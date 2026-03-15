#include <Arduino.h>
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

static uint32_t next_status_log_ms = 0;
static uint32_t wifi_disconnect_since_ms = 0;
static uint32_t next_wifi_retry_ms = 0;
static uint32_t wifi_recovered_at_ms = 0;
static bool wifi_was_connected = false;
static bool should_restart_after_reconnect = false;
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

static void setSwitchState(bool on, bool notifyHomeKit) {
  cha_switch_on.value.bool_value = on;
  setRelay(on);
  logf("Role durumu: %s", on ? "ACIK" : "KAPALI");

  if (notifyHomeKit) {
    homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
  }
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.hostname("wemos-role");
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  logf("Wi-Fi baglaniyor: %s", WIFI_SSID);

  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;

    if (attempts >= 120) {
      logf("Wi-Fi baglantisi kurulamadigi icin yeniden baslatiliyor.");
      ESP.restart();
    }
  }

  logf("Wi-Fi baglandi. IP: %s", WiFi.localIP().toString().c_str());
  wifi_was_connected = true;
  wifi_disconnect_since_ms = 0;
  next_wifi_retry_ms = 0;
  wifi_recovered_at_ms = 0;
  should_restart_after_reconnect = false;
}

void switchSetter(const homekit_value_t value) {
  const bool on = value.bool_value;
  setSwitchState(on, false);
}

static void setupHomeKit() {
  cha_switch_on.setter = switchSetter;
  arduino_homekit_setup(&config);
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
  body += "\"wifi_ssid\":\"" + String(WIFI_SSID) + "\",";
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
  body += "\"rssi\":" + String(WiFi.RSSI());
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
  body += "\"telnet\":\"telnet " + WiFi.localIP().toString() + " 23\",";
  body += "\"ota_host\":\"wemos-role.local\"";
  body += "}";
  body += "}";
  webServer.send(200, "application/json", body);
}

static void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.begin();
  logf("HTTP durum sayfasi hazir: http://%s/", WiFi.localIP().toString().c_str());
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
  } else if (wifi_disconnect_since_ms == 0) {
    wifi_disconnect_since_ms = now;
  }

  if (now >= next_wifi_retry_ms) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    next_wifi_retry_ms = now + 10000;
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

  connectWifi();
  setupOta();
  setupWebServer();
  setupHomeKit();

  logf("HomeKit hazir. Eslesme kodu: 111-11-111");
}

void loop() {
  handleWifiReconnect();
  handleTelnet();
  ArduinoOTA.handle();
  webServer.handleClient();
  arduino_homekit_loop();
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
