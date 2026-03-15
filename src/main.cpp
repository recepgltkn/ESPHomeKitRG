#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include <arduino_homekit_server.h>
#include <user_interface.h>
#include "wifi_info.h"

#ifndef APP_VERSION
#define APP_VERSION "dev-local"
#endif

extern "C" {
#include <homekit/homekit.h>
extern homekit_server_config_t config;
extern homekit_characteristic_t cha_switch_on;
}

#define SERIAL_BAUD 115200
#define RELAY_PIN D1
#define RELAY_ACTIVE_LEVEL LOW
#define RELAY_INACTIVE_LEVEL HIGH
#define HTTP_PORT 80
#define TELNET_PORT 23
#define WIFI_CONFIG_FILE "/wifi.txt"
#define WIFI_SETUP_AP "Wemos-Setup"
#define APP_NAME "Wemos Role"
#define STATUS_REFRESH_MS 3000UL
#define UPDATE_CHECK_INTERVAL_MS 60000UL
#define UPDATE_MANIFEST_URL "https://recepgltkn.github.io/ESPHomeKitRG/latest/version.json"

static uint32_t next_status_log_ms = 0;
static uint32_t wifi_disconnect_since_ms = 0;
static uint32_t next_wifi_retry_ms = 0;
static uint32_t wifi_recovered_at_ms = 0;
static uint32_t wifi_disconnect_count = 0;
static uint32_t wifi_reconnect_count = 0;
static uint32_t wifi_retry_count = 0;
static bool wifi_was_connected = false;
static bool should_restart_after_reconnect = false;
static bool setupApActive = false;
static String wifiSsid = WIFI_SSID;
static String wifiPassword = WIFI_PASSWORD;
static uint32_t lastUpdateCheckMs = 0;
static bool updateInProgress = false;
static String lastUpdateResult = "never_checked";
static String lastRemoteVersion = "";
static String lastRemoteBinUrl = "";
static bool updateAvailable = false;
static uint32_t lastSuccessfulUpdateCheckMs = 0;
static ESP8266WebServer webServer(HTTP_PORT);
static WiFiServer telnetServer(TELNET_PORT);
static WiFiClient telnetClient;

static void logf(const char *fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  Serial.println(buffer);

  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(buffer);
  }
}

static void setRelay(bool on) {
  digitalWrite(RELAY_PIN, on ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
}

static String htmlEscape(const String &value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  escaped.replace("\"", "&quot;");
  return escaped;
}

static String jsonEscape(const String &value) {
  String escaped = value;
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", "\\n");
  escaped.replace("\r", "\\r");
  return escaped;
}

static String extractJsonString(const String &json, const String &key) {
  const String token = "\"" + key + "\"";
  int keyIndex = json.indexOf(token);
  if (keyIndex < 0) {
    return "";
  }

  keyIndex = json.indexOf(':', keyIndex + token.length());
  if (keyIndex < 0) {
    return "";
  }

  int valueStart = keyIndex + 1;
  while (valueStart < static_cast<int>(json.length()) &&
         (json[valueStart] == ' ' || json[valueStart] == '\n' || json[valueStart] == '\r' || json[valueStart] == '\t')) {
    valueStart++;
  }

  if (valueStart >= static_cast<int>(json.length()) || json[valueStart] != '"') {
    return "";
  }

  valueStart++;
  const int valueEnd = json.indexOf('"', valueStart);
  if (valueEnd < 0) {
    return "";
  }

  return json.substring(valueStart, valueEnd);
}

static bool parseHttpsUrl(const String &url, String &host, uint16_t &port, String &path) {
  if (!url.startsWith("https://")) {
    return false;
  }

  const int hostStart = 8;
  const int pathStart = url.indexOf('/', hostStart);
  const String hostPort = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
  const int colonIndex = hostPort.indexOf(':');

  if (colonIndex >= 0) {
    host = hostPort.substring(0, colonIndex);
    port = static_cast<uint16_t>(hostPort.substring(colonIndex + 1).toInt());
  } else {
    host = hostPort;
    port = 443;
  }

  path = pathStart >= 0 ? url.substring(pathStart) : "/";
  return host.length() > 0 && path.length() > 0;
}

static bool readHttpResponse(BearSSL::WiFiClientSecure &client, int &statusCode, int &contentLength, bool &chunked, String &headers) {
  statusCode = -1;
  contentLength = -1;
  chunked = false;
  headers = "";

  const String statusLine = client.readStringUntil('\n');
  if (!statusLine.startsWith("HTTP/1.1 ") && !statusLine.startsWith("HTTP/1.0 ")) {
    return false;
  }

  statusCode = statusLine.substring(9, 12).toInt();

  while (client.connected()) {
    const String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) {
      break;
    }

    headers += line;
    const String trimmed = line.substring(0, line.length() - 1);
    if (trimmed.startsWith("Content-Length:")) {
      contentLength = trimmed.substring(15).toInt();
    } else if (trimmed.startsWith("Transfer-Encoding:") && trimmed.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }

  return true;
}

static bool httpsGetString(const String &url, String &body, String &error) {
  String host;
  String path;
  uint16_t port = 443;
  if (!parseHttpsUrl(url, host, port, path)) {
    error = "bad_url";
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setTimeout(15000);
  client.setInsecure();

  if (!client.connect(host.c_str(), port)) {
    error = "connect_failed";
    return false;
  }

  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: wemos-role\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str());

  int statusCode = -1;
  int contentLength = -1;
  bool chunked = false;
  String headers;
  if (!readHttpResponse(client, statusCode, contentLength, chunked, headers)) {
    error = "bad_response";
    client.stop();
    return false;
  }

  if (statusCode != 200) {
    error = "http_" + String(statusCode);
    client.stop();
    return false;
  }

  if (chunked) {
    error = "chunked_unsupported";
    client.stop();
    return false;
  }

  body = client.readString();
  client.stop();
  if (contentLength >= 0 && body.length() == 0) {
    error = "empty_body";
    return false;
  }

  return true;
}

static bool httpsUpdateFromUrl(const String &url, String &error) {
  String host;
  String path;
  uint16_t port = 443;
  if (!parseHttpsUrl(url, host, port, path)) {
    error = "bad_bin_url";
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setTimeout(20000);
  client.setInsecure();

  if (!client.connect(host.c_str(), port)) {
    error = "bin_connect_failed";
    return false;
  }

  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: wemos-role\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str());

  int statusCode = -1;
  int contentLength = -1;
  bool chunked = false;
  String headers;
  if (!readHttpResponse(client, statusCode, contentLength, chunked, headers)) {
    error = "bin_bad_response";
    client.stop();
    return false;
  }

  if (statusCode != 200) {
    error = "bin_http_" + String(statusCode);
    client.stop();
    return false;
  }

  if (chunked || contentLength <= 0) {
    error = "bin_length_missing";
    client.stop();
    return false;
  }

  if (!Update.begin(contentLength)) {
    error = "update_begin_failed";
    client.stop();
    return false;
  }

  logf("Otomatik update basladi. Boyut: %d", contentLength);
  uint8_t buffer[1024];
  size_t totalWritten = 0;
  uint32_t lastDataMs = millis();

  while (totalWritten < static_cast<size_t>(contentLength)) {
    const size_t availableBytes = client.available();
    if (availableBytes == 0) {
      if (!client.connected() && !client.available()) {
        break;
      }

      if (millis() - lastDataMs > 15000) {
        error = "update_read_timeout";
        Update.end();
        client.stop();
        return false;
      }

      delay(1);
      continue;
    }

    const size_t chunkSize = availableBytes > sizeof(buffer) ? sizeof(buffer) : availableBytes;
    const int bytesRead = client.read(buffer, chunkSize);
    if (bytesRead <= 0) {
      continue;
    }

    lastDataMs = millis();
    const size_t bytesWritten = Update.write(buffer, bytesRead);
    totalWritten += bytesWritten;
    if (bytesWritten != static_cast<size_t>(bytesRead)) {
      error = "write_mismatch_" + String(totalWritten) + "_" + String(contentLength);
      Update.end();
      client.stop();
      return false;
    }
  }

  if (!Update.end()) {
    error = "update_end_failed";
    client.stop();
    return false;
  }

  if (totalWritten != static_cast<size_t>(contentLength)) {
    error = "write_incomplete_" + String(totalWritten) + "_" + String(contentLength);
    client.stop();
    return false;
  }

  if (!Update.isFinished()) {
    error = "update_not_finished";
    client.stop();
    return false;
  }

  client.stop();
  return true;
}

static void loadWifiCredentials() {
  if (!LittleFS.begin()) {
    logf("LittleFS baslatilamadi, derlenmis Wi-Fi bilgileri kullaniliyor.");
    return;
  }

  if (!LittleFS.exists(WIFI_CONFIG_FILE)) {
    return;
  }

  File file = LittleFS.open(WIFI_CONFIG_FILE, "r");
  if (!file) {
    return;
  }

  wifiSsid = file.readStringUntil('\n');
  wifiSsid.trim();
  wifiPassword = file.readStringUntil('\n');
  wifiPassword.trim();
  file.close();

  if (wifiSsid.length() == 0) {
    wifiSsid = WIFI_SSID;
    wifiPassword = WIFI_PASSWORD;
  }
}

static bool saveWifiCredentials(const String &ssid, const String &password) {
  File file = LittleFS.open(WIFI_CONFIG_FILE, "w");
  if (!file) {
    return false;
  }

  file.println(ssid);
  file.println(password);
  file.close();
  wifiSsid = ssid;
  wifiPassword = password;
  return true;
}

static void startSetupAccessPoint() {
  if (setupApActive) {
    return;
  }

  WiFi.softAP(WIFI_SETUP_AP);
  setupApActive = true;
  logf("Setup AP aktif: %s / http://192.168.4.1/setup", WIFI_SETUP_AP);
}

static void stopSetupAccessPoint() {
  if (!setupApActive) {
    return;
  }

  WiFi.softAPdisconnect(true);
  setupApActive = false;
  logf("Setup AP kapatildi.");
}

static String buildStatusJson() {
  String body;
  body.reserve(3072);
  body += "{";
  body += "\"device\":{";
  body += "\"name\":\"" APP_NAME "\",";
  body += "\"model\":\"D1MiniRelay\",";
  body += "\"firmware\":\"" APP_VERSION "\",";
  body += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\",";
  body += "\"flash_chip_id\":\"" + String(ESP.getFlashChipId(), HEX) + "\",";
  body += "\"cpu_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  body += "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"";
  body += "},";
  body += "\"network\":{";
  body += "\"hostname\":\"wemos-role\",";
  body += "\"wifi_ssid\":\"" + jsonEscape(wifiSsid) + "\",";
  body += "\"status\":" + String(WiFi.status()) + ",";
  body += "\"connected\":";
  body += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  body += ",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"gateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  body += "\"subnet\":\"" + WiFi.subnetMask().toString() + "\",";
  body += "\"dns\":\"" + WiFi.dnsIP().toString() + "\",";
  body += "\"mac\":\"" + WiFi.macAddress() + "\",";
  body += "\"bssid\":\"" + WiFi.BSSIDstr() + "\",";
  body += "\"channel\":" + String(WiFi.channel()) + ",";
  body += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  body += "\"sleep_mode\":\"NONE\",";
  body += "\"auto_reconnect\":true,";
  body += "\"disconnect_count\":" + String(wifi_disconnect_count) + ",";
  body += "\"reconnect_count\":" + String(wifi_reconnect_count) + ",";
  body += "\"retry_count\":" + String(wifi_retry_count) + ",";
  body += "\"disconnect_duration_ms\":" + String(wifi_disconnect_since_ms == 0 ? 0 : millis() - wifi_disconnect_since_ms) + ",";
  body += "\"setup_ap_active\":";
  body += setupApActive ? "true" : "false";
  body += "},";
  body += "\"relay\":{";
  body += "\"on\":";
  body += cha_switch_on.value.bool_value ? "true" : "false";
  body += ",";
  body += "\"gpio\":" + String(RELAY_PIN);
  body += "},";
  body += "\"homekit\":{";
  body += "\"clients\":" + String(arduino_homekit_connected_clients_count()) + ",";
  body += "\"pairing_code\":\"111-11-111\"";
  body += "},";
  body += "\"update\":{";
  body += "\"auto_update_enabled\":true,";
  body += "\"current_version\":\"" APP_VERSION "\",";
  body += "\"last_remote_version\":\"" + jsonEscape(lastRemoteVersion) + "\",";
  body += "\"last_remote_bin_url\":\"" + jsonEscape(lastRemoteBinUrl) + "\",";
  body += "\"manifest_url\":\"" UPDATE_MANIFEST_URL "\",";
  body += "\"check_interval_ms\":" + String(UPDATE_CHECK_INTERVAL_MS) + ",";
  body += "\"last_check_ms_ago\":" + String(lastUpdateCheckMs == 0 ? 0 : millis() - lastUpdateCheckMs) + ",";
  body += "\"last_successful_check_ms_ago\":" + String(lastSuccessfulUpdateCheckMs == 0 ? 0 : millis() - lastSuccessfulUpdateCheckMs) + ",";
  body += "\"next_check_in_ms\":" + String(lastUpdateCheckMs == 0 ? 0 : (UPDATE_CHECK_INTERVAL_MS > (millis() - lastUpdateCheckMs) ? UPDATE_CHECK_INTERVAL_MS - (millis() - lastUpdateCheckMs) : 0)) + ",";
  body += "\"in_progress\":";
  body += updateInProgress ? "true" : "false";
  body += ",";
  body += "\"available\":";
  body += updateAvailable ? "true" : "false";
  body += ",";
  body += "\"last_result\":\"" + jsonEscape(lastUpdateResult) + "\"";
  body += "},";
  body += "\"system\":{";
  body += "\"uptime_ms\":" + String(millis()) + ",";
  body += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  body += "\"heap_fragmentation\":" + String(ESP.getHeapFragmentation()) + ",";
  body += "\"max_free_block\":" + String(ESP.getMaxFreeBlockSize()) + ",";
  body += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  body += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  body += "\"reset_reason\":\"" + jsonEscape(ESP.getResetReason()) + "\"";
  body += "},";
  body += "\"services\":{";
  body += "\"http\":\"http://" + WiFi.localIP().toString() + "/\",";
  body += "\"status\":\"http://" + WiFi.localIP().toString() + "/status\",";
  body += "\"api_status\":\"http://" + WiFi.localIP().toString() + "/api/status\",";
  body += "\"setup\":\"";
  body += (WiFi.status() == WL_CONNECTED) ? "http://" + WiFi.localIP().toString() + "/setup" : "http://192.168.4.1/setup";
  body += "\",";
  body += "\"telnet\":\"telnet " + WiFi.localIP().toString() + " 23\",";
  body += "\"ota_host\":\"wemos-role.local\"";
  body += "}";
  body += "}";
  return body;
}

static void setSwitchState(bool on, bool notifyHomeKit) {
  cha_switch_on.value.bool_value = on;
  setRelay(on);
  logf("Role durumu: %s", on ? "ACIK" : "KAPALI");

  if (notifyHomeKit) {
    homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
  }
}

static bool connectWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.hostname("wemos-role");
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  startSetupAccessPoint();
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  logf("Wi-Fi baglaniyor: %s", wifiSsid.c_str());

  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;

    if (attempts >= 40) {
      logf("Wi-Fi baglantisi kurulamadi. Setup AP acik kalacak.");
      return false;
    }
  }

  logf("Wi-Fi baglandi. IP: %s", WiFi.localIP().toString().c_str());
  stopSetupAccessPoint();
  wifi_was_connected = true;
  wifi_disconnect_since_ms = 0;
  next_wifi_retry_ms = 0;
  wifi_recovered_at_ms = 0;
  should_restart_after_reconnect = false;
  return true;
}

void switchSetter(const homekit_value_t value) {
  const bool on = value.bool_value;
  setSwitchState(on, false);
}

static void setupHomeKit() {
  cha_switch_on.setter = switchSetter;
  arduino_homekit_setup(&config);
}

static void handleSetupPage() {
  int networkCount = WiFi.scanComplete();
  if (networkCount == WIFI_SCAN_FAILED || networkCount == WIFI_SCAN_RUNNING) {
    WiFi.scanNetworks(true, true);
    networkCount = 0;
  }

  String body;
  body.reserve(4096);
  body += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>Wemos Setup</title><style>";
  body += "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;background:#f6f7f8;color:#111}";
  body += "form,section{background:#fff;padding:16px 18px;border-radius:12px;box-shadow:0 4px 24px rgba(0,0,0,.06);margin-bottom:16px}";
  body += "input,select,button{width:100%;padding:12px;margin:8px 0;border:1px solid #d0d7de;border-radius:8px;font-size:16px}";
  body += "button{background:#111;color:#fff;border:none}";
  body += "small{color:#666} ul{padding-left:18px}";
  body += "</style></head><body>";
  body += "<h1>Wemos Wi-Fi Kurulumu</h1>";
  body += "<section><strong>Setup AP:</strong> ";
  body += WIFI_SETUP_AP;
  body += "<br><small>Bu sayfa cihaz mevcut ağa bağlanamadığında açılır.</small></section>";
  body += "<form method='POST' action='/setup'>";
  body += "<label>SSID</label>";
  body += "<input name='ssid' list='ssid-list' value='" + htmlEscape(wifiSsid) + "' required>";
  body += "<datalist id='ssid-list'>";
  for (int i = 0; i < networkCount; i++) {
    body += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>";
  }
  body += "</datalist>";
  body += "<label>Sifre</label>";
  body += "<input name='password' type='password' value='" + htmlEscape(wifiPassword) + "'>";
  body += "<button type='submit'>Kaydet ve Yeniden Baslat</button>";
  body += "</form>";
  body += "<section><h2>Yakin Aglar</h2><ul>";
  for (int i = 0; i < networkCount; i++) {
    body += "<li>";
    body += htmlEscape(WiFi.SSID(i));
    body += " (RSSI ";
    body += String(WiFi.RSSI(i));
    body += ", kanal ";
    body += String(WiFi.channel(i));
    body += WiFi.encryptionType(i) == ENC_TYPE_NONE ? ", acik" : ", sifreli";
    body += ")</li>";
  }
  body += "</ul></section></body></html>";
  webServer.send(200, "text/html; charset=utf-8", body);
}

static void handleSetupSave() {
  const String ssid = webServer.arg("ssid");
  const String password = webServer.arg("password");

  if (ssid.length() == 0) {
    webServer.send(400, "text/plain", "SSID zorunlu.");
    return;
  }

  if (!saveWifiCredentials(ssid, password)) {
    webServer.send(500, "text/plain", "Wi-Fi bilgileri kaydedilemedi.");
    return;
  }

  webServer.send(200, "text/html; charset=utf-8",
                 "<html><body><h1>Kaydedildi</h1><p>Cihaz yeniden baslatiliyor...</p></body></html>");
  delay(1000);
  ESP.restart();
}

static void handleRoot() {
  webServer.send(200, "application/json", buildStatusJson());
}

static void handleApiStatus() {
  webServer.send(200, "application/json", buildStatusJson());
}

static void handleStatusPage() {
  String body;
  body.reserve(5000);
  body += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>Wemos Status</title><style>";
  body += ":root{--bg:#f5f1e8;--card:#fffdf7;--ink:#181611;--muted:#6f675c;--line:#ddd3c2;--accent:#1f6f5f;--warn:#9a3412}";
  body += "body{margin:0;background:radial-gradient(circle at top,#fffaf0 0,#f5f1e8 48%,#ece6d8 100%);color:var(--ink);font-family:Georgia,serif}";
  body += ".wrap{max-width:1100px;margin:0 auto;padding:24px}.hero{display:flex;justify-content:space-between;gap:16px;align-items:end;margin-bottom:18px}";
  body += "h1{margin:0;font-size:42px}.sub{color:var(--muted)}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}";
  body += ".card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.05)}";
  body += ".label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}.value{font-size:28px;margin-top:6px}.mono{font-family:ui-monospace,monospace;font-size:14px}";
  body += ".good{color:var(--accent)}.warn{color:var(--warn)} a{color:var(--accent);text-decoration:none}.row{display:flex;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px solid var(--line)}";
  body += ".row:first-child{border-top:none}.pill{display:inline-block;border-radius:999px;padding:6px 10px;background:#ece6d8;font-size:12px}.footer{margin-top:16px;color:var(--muted)}";
  body += "</style></head><body><div class='wrap'>";
  body += "<div class='hero'><div><div class='sub'>Canli cihaz paneli</div><h1>" APP_NAME "</h1></div><div class='pill'>Otomatik yenileme: 3 sn</div></div>";
  body += "<div class='grid'>";
  body += "<div class='card'><div class='label'>Role</div><div id='relay' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Wi-Fi</div><div id='wifi' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>RSSI</div><div id='rssi' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Heap</div><div id='heap' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Version</div><div id='version' class='value mono'>-</div></div>";
  body += "<div class='card'><div class='label'>Update</div><div id='update' class='value mono'>-</div></div>";
  body += "<div class='card'><div class='label'>Sonraki Kontrol</div><div id='nextCheck' class='value mono'>-</div></div>";
  body += "</div>";
  body += "<div class='grid' style='margin-top:14px'>";
  body += "<div class='card'><div class='label'>Ag Detayi</div><div id='networkRows'></div></div>";
  body += "<div class='card'><div class='label'>Sistem</div><div id='systemRows'></div></div>";
  body += "<div class='card'><div class='label'>Servisler</div><div id='serviceRows'></div></div>";
  body += "</div>";
  body += "<div class='footer'>Kaynak: <a href='/api/status'>/api/status</a></div>";
  body += "</div><script>";
  body += "const set=(id,v,cls='')=>{const e=document.getElementById(id);e.textContent=v;e.className='value '+cls;};";
  body += "const rows=(id,data)=>{const e=document.getElementById(id);e.innerHTML=data.map(([k,v])=>`<div class='row'><span>${k}</span><span class='mono'>${v}</span></div>`).join('');};";
  body += "async function load(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();";
  body += "set('relay',d.relay.on?'ACIK':'KAPALI',d.relay.on?'good':'warn');";
  body += "set('wifi',d.network.connected?'BAGLI':'KOPUK',d.network.connected?'good':'warn');";
  body += "set('rssi',`${d.network.rssi} dBm`,d.network.rssi>-70?'good':'warn');";
  body += "set('heap',`${d.system.free_heap} B`,d.system.free_heap>20000?'good':'warn');";
  body += "set('version',d.update.current_version,'mono');";
  body += "set('update',d.update.in_progress?'YUKLENIYOR':d.update.last_result,'mono');";
  body += "set('nextCheck',`${Math.ceil(d.update.next_check_in_ms/1000)} sn`,'mono');";
  body += "rows('networkRows',[['SSID',d.network.wifi_ssid],['IP',d.network.ip],['BSSID',d.network.bssid],['Kanal',d.network.channel],['RSSI',`${d.network.rssi} dBm`],['Reconnect',d.network.reconnect_count],['Retry',d.network.retry_count],['Setup AP',d.network.setup_ap_active?'ACIK':'KAPALI']]);";
  body += "rows('systemRows',[['Uptime(ms)',d.system.uptime_ms],['Reset',d.system.reset_reason],['Fragmentation',d.system.heap_fragmentation],['Max block',d.system.max_free_block],['Sketch',d.system.sketch_size],['Clients',d.homekit.clients],['Son kontrol',`${Math.floor(d.update.last_check_ms_ago/1000)} sn once`],['Basarili kontrol',`${Math.floor(d.update.last_successful_check_ms_ago/1000)} sn once`]]);";
  body += "rows('serviceRows',[['Setup',d.services.setup],['HTTP',d.services.http],['Status',d.services.status],['Telnet',d.services.telnet],['OTA Host',d.services.ota_host],['Manifest',d.update.manifest_url],['Yeni surum',d.update.last_remote_version||'-']]);";
  body += "}catch(e){set('update','baglanti hatasi','warn');}} load(); setInterval(load," + String(STATUS_REFRESH_MS) + ");";
  body += "</script></body></html>";
  webServer.send(200, "text/html; charset=utf-8", body);
}

static void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/api/status", handleApiStatus);
  webServer.on("/status", handleStatusPage);
  webServer.on("/setup", HTTP_GET, handleSetupPage);
  webServer.on("/setup", HTTP_POST, handleSetupSave);
  webServer.begin();
  logf("HTTP hazir: http://%s/", WiFi.localIP().toString().c_str());
}

static void setupOta() {
  ArduinoOTA.setHostname("wemos-role");
  ArduinoOTA.onStart([]() {
    logf("OTA basladi.");
  });
  ArduinoOTA.onEnd([]() {
    logf("OTA tamamlandi.");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    logf("OTA hata: %u", error);
  });
  ArduinoOTA.begin();
  logf("OTA hazir: wemos-role.local");
}

static void handleTelnet() {
  if (telnetServer.hasClient()) {
    WiFiClient candidate = telnetServer.available();
    if (!telnetClient || !telnetClient.connected()) {
      telnetClient = candidate;
      telnetClient.println("Wemos Role log baglantisi kuruldu.");
      logf("Telnet istemcisi baglandi.");
    } else {
      candidate.stop();
    }
  }

  if (telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
  }
}

static void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED || updateInProgress) {
    return;
  }

  const uint32_t now = millis();
  if (lastUpdateCheckMs != 0 && now - lastUpdateCheckMs < UPDATE_CHECK_INTERVAL_MS) {
    return;
  }
  lastUpdateCheckMs = now;

  String payload;
  String requestError;
  if (!httpsGetString(UPDATE_MANIFEST_URL, payload, requestError)) {
    lastUpdateResult = "manifest_" + requestError;
    return;
  }
  lastSuccessfulUpdateCheckMs = now;

  const String remoteVersion = extractJsonString(payload, "version");
  const String remoteBinUrl = extractJsonString(payload, "bin_url");
  lastRemoteVersion = remoteVersion;
  lastRemoteBinUrl = remoteBinUrl;

  if (remoteVersion.length() == 0 || remoteBinUrl.length() == 0) {
    lastUpdateResult = "manifest_parse_failed";
    return;
  }

  if (remoteVersion == APP_VERSION) {
    updateAvailable = false;
    lastUpdateResult = "up_to_date";
    return;
  }

  updateAvailable = true;
  updateInProgress = true;
  lastUpdateResult = "updating";
  logf("Yeni surum bulundu: %s", remoteVersion.c_str());
  String updateError;
  if (httpsUpdateFromUrl(remoteBinUrl, updateError)) {
    lastUpdateResult = "update_ok";
    logf("Otomatik update tamamlandi. Yeniden baslatiliyor.");
    delay(500);
    ESP.restart();
    return;
  }

  updateInProgress = false;
  lastUpdateResult = "update_" + updateError;
  logf("Otomatik update hatasi: %s", updateError.c_str());
}

static void handleWifiReconnect() {
  const wl_status_t status = WiFi.status();
  const uint32_t now = millis();

  if (status == WL_CONNECTED) {
    if (!wifi_was_connected) {
      logf("Wi-Fi yeniden baglandi. IP: %s", WiFi.localIP().toString().c_str());
      wifi_was_connected = true;
      wifi_recovered_at_ms = now;
      should_restart_after_reconnect = true;
      wifi_reconnect_count++;
    }
    wifi_disconnect_since_ms = 0;
    next_wifi_retry_ms = 0;

    if (should_restart_after_reconnect && wifi_recovered_at_ms != 0 && now - wifi_recovered_at_ms >= 2000) {
      logf("Wi-Fi geri geldi. HomeKit servisini temiz toparlamak icin cihaz yeniden baslatiliyor.");
      ESP.restart();
    }
    return;
  }

  if (wifi_was_connected) {
    logf("Wi-Fi baglantisi koptu. Yeniden baglaniliyor...");
    wifi_was_connected = false;
    wifi_disconnect_since_ms = now;
    wifi_disconnect_count++;
  } else if (wifi_disconnect_since_ms == 0) {
    wifi_disconnect_since_ms = now;
  }

  if (now >= next_wifi_retry_ms) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    next_wifi_retry_ms = now + 10000;
    wifi_retry_count++;
    logf("Wi-Fi yeniden baglanma denemesi baslatildi.");
  }

  if (wifi_disconnect_since_ms != 0 && now - wifi_disconnect_since_ms >= 60000) {
    logf("Wi-Fi 60 saniyedir yok. Cihaz yeniden baslatiliyor.");
    ESP.restart();
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  logf("D1 Mini HomeKit role baslatiliyor...");

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  loadWifiCredentials();
  const bool wifiConnected = connectWifi();

  setupWebServer();
  if (wifiConnected) {
    setupOta();
    setupHomeKit();
  } else {
    WiFi.scanNetworks(true, true);
  }

  logf("HomeKit hazir. Eslesme kodu: 111-11-111");
}

void loop() {
  handleWifiReconnect();
  handleTelnet();
  webServer.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    checkForUpdates();
    ArduinoOTA.handle();
    arduino_homekit_loop();
  }
  delay(10);

  const uint32_t now = millis();
  if (now >= next_status_log_ms) {
    next_status_log_ms = now + 10000;
    logf(
      "Heap: %u, HomeKit istemci: %d, Role: %s",
      ESP.getFreeHeap(),
      arduino_homekit_connected_clients_count(),
      cha_switch_on.value.bool_value ? "ACIK" : "KAPALI"
    );
  }
}
