#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <arduino_homekit_server.h>
#include <user_interface.h>
#include "wifi_info.h"

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
  String body;
  body.reserve(2048);
  body += "{";
  body += "\"device\":{";
  body += "\"name\":\"Wemos Role\",";
  body += "\"model\":\"D1MiniRelay\",";
  body += "\"firmware\":\"1.0.0\",";
  body += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\",";
  body += "\"flash_chip_id\":\"" + String(ESP.getFlashChipId(), HEX) + "\",";
  body += "\"cpu_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  body += "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"";
  body += "},";
  body += "\"network\":{";
  body += "\"hostname\":\"wemos-role\",";
  body += "\"wifi_ssid\":\"" + wifiSsid + "\",";
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
  body += "\"disconnect_duration_ms\":" + String(wifi_disconnect_since_ms == 0 ? 0 : millis() - wifi_disconnect_since_ms);
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
  body += "\"system\":{";
  body += "\"uptime_ms\":" + String(millis()) + ",";
  body += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  body += "\"heap_fragmentation\":" + String(ESP.getHeapFragmentation()) + ",";
  body += "\"max_free_block\":" + String(ESP.getMaxFreeBlockSize()) + ",";
  body += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  body += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  body += "\"reset_reason\":\"" + ESP.getResetReason() + "\"";
  body += "},";
  body += "\"services\":{";
  body += "\"http\":\"http://" + WiFi.localIP().toString() + "/\",";
  body += "\"setup\":\"";
  body += (WiFi.status() == WL_CONNECTED) ? "http://" + WiFi.localIP().toString() + "/setup" : "http://192.168.4.1/setup";
  body += "\",";
  body += "\"telnet\":\"telnet " + WiFi.localIP().toString() + " 23\",";
  body += "\"ota_host\":\"wemos-role.local\"";
  body += "}";
  body += "}";
  webServer.send(200, "application/json", body);
}

static void setupWebServer() {
  webServer.on("/", handleRoot);
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
