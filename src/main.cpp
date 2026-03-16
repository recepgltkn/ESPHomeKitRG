#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include <DHTesp.h>
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
extern homekit_characteristic_t cha_switch_brightness;
extern homekit_characteristic_t cha_name;
extern homekit_characteristic_t cha_accessory_name;
extern homekit_characteristic_t cha_serial_number;
extern homekit_characteristic_t cha_firmware_revision;
extern homekit_characteristic_t cha_temperature;
extern homekit_characteristic_t cha_humidity;
extern homekit_characteristic_t cha_motion_detected;
extern homekit_characteristic_t cha_light_level;
extern homekit_characteristic_t cha_aux1_on;
extern homekit_characteristic_t cha_aux1_brightness;
extern homekit_characteristic_t cha_aux2_on;
extern homekit_characteristic_t cha_aux2_brightness;
}

#define SERIAL_BAUD 115200
#define RELAY_PIN D1
#define RELAY_ACTIVE_LEVEL HIGH
#define RELAY_INACTIVE_LEVEL LOW
#define BUTTON_PIN D2
#define PIR_PIN D5
#define DHT_PIN D6
#define AUX1_PWM_PIN D7
#define AUX2_PWM_PIN D8
#define LDR_PIN A0
#define HTTP_PORT 80
#define TELNET_PORT 23
#define WIFI_CONFIG_FILE "/wifi.txt"
#define SENSOR_CONFIG_FILE "/config.txt"
#define WIFI_SETUP_AP_PREFIX "Wemos-Setup"
#define WIFI_SETUP_PASSWORD "12345678"
#define APP_NAME "Wemos Role"
#define STATUS_REFRESH_MS 3000UL
#define UPDATE_CHECK_INTERVAL_MS 60000UL
#define AUTO_UPDATE_ENABLED false
#define UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/recepgltkn/ESPHomeKitRG/main/docs/latest/version.json"
#define HOMEKIT_RECOVERY_IDLE_MS 180000UL
#define HOMEKIT_RECOVERY_GRACE_MS 120000UL
#define HOMEKIT_RECOVERY_COOLDOWN_MS 600000UL
#define SENSOR_READ_INTERVAL_MS 5000UL

struct SensorConfig {
  uint16_t ldr_threshold;
  uint16_t ldr_hysteresis;
  bool pir_enabled;
  uint32_t pir_hold_ms;
  uint16_t pir_cooldown_ms;
  int8_t temperature_offset_tenths;
  int8_t humidity_offset_tenths;
  uint16_t button_debounce_ms;
  bool aux1_inverted;
  bool aux2_inverted;
  uint8_t aux1_default_brightness;
  uint8_t aux2_default_brightness;
};

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
static String lastRemoteBuildTime = "";
static String lastRemoteCommit = "";
static String lastRemoteNotes = "";
static bool updateAvailable = false;
static uint32_t lastSuccessfulUpdateCheckMs = 0;
static uint32_t homekitLastClientSeenMs = 0;
static uint32_t homekitLastRecoveryRestartMs = 0;
static uint32_t lastSensorReadMs = 0;
static uint32_t lastButtonToggleMs = 0;
static uint32_t pirLastMotionMs = 0;
static uint32_t pirLastTransitionMs = 0;
static bool homekitEverHadClient = false;
static bool homekitRecoveryPending = false;
static bool buttonPressed = false;
static bool pirMotionState = false;
static bool ldrDarkState = false;
static char deviceName[32] = APP_NAME;
static char otaHostname[32] = "wemos-role";
static char serialNumber[32] = "WEMOS-D1-R2-RELAY";
static char firmwareRevision[16] = APP_VERSION;
static char setupApName[32] = WIFI_SETUP_AP_PREFIX;
static ESP8266WebServer webServer(HTTP_PORT);
static WiFiServer telnetServer(TELNET_PORT);
static WiFiClient telnetClient;
static DHTesp dht;
static SensorConfig sensorConfig = {
  550,
  40,
  false,
  30000,
  2500,
  0,
  0,
  250,
  false,
  false,
  100,
  100
};
static float lastTemperatureC = 20.0f;
static float lastHumidityPct = 50.0f;
static uint16_t lastLdrRaw = 0;
static float lastLux = 10.0f;
static void setSwitchState(bool on, bool notifyHomeKit);
static void setMainBrightness(int value, bool notifyHomeKit);
static void applyAuxOutput(uint8_t channel, bool on, int brightness, bool notifyHomeKit);

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

static int brightnessToPwm(const int brightness, const bool inverted) {
  const int safeBrightness = brightness < 0 ? 0 : (brightness > 100 ? 100 : brightness);
  int pwm = map(safeBrightness, 0, 100, 0, 1023);
  if (inverted) {
    pwm = 1023 - pwm;
  }
  return pwm;
}

static void applyAuxOutput(uint8_t channel, bool on, int brightness, bool notifyHomeKit) {
  const int effectiveBrightness = on ? (brightness < 1 ? 1 : brightness) : 0;
  const int pwmValue = brightnessToPwm(effectiveBrightness, channel == 1 ? sensorConfig.aux1_inverted : sensorConfig.aux2_inverted);

  if (channel == 1) {
    analogWrite(AUX1_PWM_PIN, pwmValue);
    cha_aux1_on.value.bool_value = on;
    cha_aux1_brightness.value.int_value = on ? effectiveBrightness : 0;
    if (notifyHomeKit) {
      homekit_characteristic_notify(&cha_aux1_on, cha_aux1_on.value);
      homekit_characteristic_notify(&cha_aux1_brightness, cha_aux1_brightness.value);
    }
    return;
  }

  analogWrite(AUX2_PWM_PIN, pwmValue);
  cha_aux2_on.value.bool_value = on;
  cha_aux2_brightness.value.int_value = on ? effectiveBrightness : 0;
  if (notifyHomeKit) {
    homekit_characteristic_notify(&cha_aux2_on, cha_aux2_on.value);
    homekit_characteristic_notify(&cha_aux2_brightness, cha_aux2_brightness.value);
  }
}

static void prepareDeviceIdentity() {
  const unsigned long chipId = ESP.getChipId();
  snprintf(deviceName, sizeof(deviceName), "%s %06lX", APP_NAME, chipId);
  snprintf(otaHostname, sizeof(otaHostname), "wemos-role-%06lx", chipId);
  snprintf(serialNumber, sizeof(serialNumber), "WEMOS-%06lX", chipId);
  snprintf(firmwareRevision, sizeof(firmwareRevision), "%s", APP_VERSION);
  snprintf(setupApName, sizeof(setupApName), "%s-%06lX", WIFI_SETUP_AP_PREFIX, chipId);

  cha_name.value.format = homekit_format_string;
  cha_name.value.string_value = deviceName;
  cha_accessory_name.value.format = homekit_format_string;
  cha_accessory_name.value.string_value = deviceName;
  cha_serial_number.value.format = homekit_format_string;
  cha_serial_number.value.string_value = serialNumber;
  cha_firmware_revision.value.format = homekit_format_string;
  cha_firmware_revision.value.string_value = firmwareRevision;
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

static String extractJsonValue(const String &json, const String &key) {
  String value = extractJsonString(json, key);
  if (value.length() > 0) {
    return value;
  }

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

  int valueEnd = valueStart;
  while (valueEnd < static_cast<int>(json.length()) &&
         json[valueEnd] != ',' && json[valueEnd] != '\n' && json[valueEnd] != '\r' && json[valueEnd] != '}') {
    valueEnd++;
  }

  return json.substring(valueStart, valueEnd);
}

static uint16_t clampU16(long value, uint16_t minValue, uint16_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return static_cast<uint16_t>(value);
}

static uint32_t clampU32(long value, uint32_t minValue, uint32_t maxValue) {
  if (value < static_cast<long>(minValue)) {
    return minValue;
  }
  if (value > static_cast<long>(maxValue)) {
    return maxValue;
  }
  return static_cast<uint32_t>(value);
}

static uint8_t clampU8(long value, uint8_t minValue, uint8_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return static_cast<uint8_t>(value);
}

static int8_t clampI8(long value, int8_t minValue, int8_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return static_cast<int8_t>(value);
}

static String readConfigLine(File &file) {
  String line = file.readStringUntil('\n');
  line.trim();
  return line;
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

static void loadSensorConfig() {
  if (!LittleFS.begin()) {
    return;
  }

  if (!LittleFS.exists(SENSOR_CONFIG_FILE)) {
    return;
  }

  File file = LittleFS.open(SENSOR_CONFIG_FILE, "r");
  if (!file) {
    return;
  }

  String lines[12];
  uint8_t lineCount = 0;
  while (file.available() && lineCount < 12) {
    lines[lineCount++] = readConfigLine(file);
  }
  file.close();

  if (lineCount < 10) {
    return;
  }

  sensorConfig.ldr_threshold = clampU16(lines[0].toInt(), 0, 1023);
  sensorConfig.ldr_hysteresis = clampU16(lines[1].toInt(), 0, 400);

  uint8_t index = 2;
  if (lineCount >= 11) {
    sensorConfig.pir_enabled = lines[index++] == "1";
  }

  sensorConfig.pir_hold_ms = clampU32(lines[index++].toInt(), 1000, 120000);
  sensorConfig.pir_cooldown_ms = clampU16(lines[index++].toInt(), 100, 30000);
  sensorConfig.temperature_offset_tenths = clampI8(lines[index++].toInt(), -100, 100);
  sensorConfig.humidity_offset_tenths = clampI8(lines[index++].toInt(), -100, 100);
  sensorConfig.button_debounce_ms = clampU16(lines[index++].toInt(), 50, 2000);
  sensorConfig.aux1_inverted = lines[index++] == "1";
  sensorConfig.aux2_inverted = lines[index++] == "1";
  sensorConfig.aux1_default_brightness = clampU8(lines[index++].toInt(), 0, 100);
  sensorConfig.aux2_default_brightness = clampU8(lines[index++].toInt(), 0, 100);
}

static bool saveSensorConfig() {
  File file = LittleFS.open(SENSOR_CONFIG_FILE, "w");
  if (!file) {
    return false;
  }

  file.println(sensorConfig.ldr_threshold);
  file.println(sensorConfig.ldr_hysteresis);
  file.println(sensorConfig.pir_enabled ? 1 : 0);
  file.println(sensorConfig.pir_hold_ms);
  file.println(sensorConfig.pir_cooldown_ms);
  file.println(sensorConfig.temperature_offset_tenths);
  file.println(sensorConfig.humidity_offset_tenths);
  file.println(sensorConfig.button_debounce_ms);
  file.println(sensorConfig.aux1_inverted ? 1 : 0);
  file.println(sensorConfig.aux2_inverted ? 1 : 0);
  file.println(sensorConfig.aux1_default_brightness);
  file.println(sensorConfig.aux2_default_brightness);
  file.close();
  return true;
}

static void startSetupAccessPoint() {
  if (setupApActive) {
    return;
  }

  WiFi.softAP(setupApName, WIFI_SETUP_PASSWORD, 1, false);
  setupApActive = true;
  logf("Setup AP aktif: %s / http://192.168.4.1/setup", setupApName);
}

static void stopSetupAccessPoint() {
  if (!setupApActive) {
    return;
  }

  WiFi.softAPdisconnect(true);
  setupApActive = false;
  logf("Setup AP kapatildi.");
}

static float clampFloatValue(float value, float minValue, float maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

static float ldrRawToLux(const uint16_t raw) {
  const float ratio = static_cast<float>(raw) / 1023.0f;
  const float lux = 0.1f + (ratio * ratio * 9999.9f);
  return clampFloatValue(lux, 0.1f, 10000.0f);
}

static void updateLightState(const uint16_t rawValue, const bool notifyHomeKit) {
  lastLdrRaw = rawValue;
  lastLux = ldrRawToLux(rawValue);

  const uint16_t darkOn = sensorConfig.ldr_threshold;
  const uint16_t darkOff = darkOn > sensorConfig.ldr_hysteresis
    ? darkOn - sensorConfig.ldr_hysteresis
    : 0;

  if (!ldrDarkState && rawValue >= darkOn) {
    ldrDarkState = true;
  } else if (ldrDarkState && rawValue <= darkOff) {
    ldrDarkState = false;
  }

  if (fabsf(cha_light_level.value.float_value - lastLux) >= 0.5f) {
    cha_light_level.value.float_value = lastLux;
    if (notifyHomeKit) {
      homekit_characteristic_notify(&cha_light_level, cha_light_level.value);
    }
  }
}

static void updateMotionState(const bool motionDetected, const bool notifyHomeKit) {
  if (pirMotionState == motionDetected) {
    return;
  }

  pirMotionState = motionDetected;
  cha_motion_detected.value.bool_value = motionDetected;
  logf("PIR durumu: %s", motionDetected ? "HAREKET" : "BOS");

  if (notifyHomeKit) {
    homekit_characteristic_notify(&cha_motion_detected, cha_motion_detected.value);
  }
}

static void updateClimateState(const float temperatureC, const float humidityPct, const bool notifyHomeKit) {
  const float clampedTemperature = clampFloatValue(temperatureC, -40.0f, 100.0f);
  const float clampedHumidity = clampFloatValue(humidityPct, 0.0f, 100.0f);

  if (fabsf(cha_temperature.value.float_value - clampedTemperature) >= 0.1f) {
    cha_temperature.value.float_value = clampedTemperature;
    if (notifyHomeKit) {
      homekit_characteristic_notify(&cha_temperature, cha_temperature.value);
    }
  }

  if (fabsf(cha_humidity.value.float_value - clampedHumidity) >= 0.5f) {
    cha_humidity.value.float_value = clampedHumidity;
    if (notifyHomeKit) {
      homekit_characteristic_notify(&cha_humidity, cha_humidity.value);
    }
  }

  lastTemperatureC = clampedTemperature;
  lastHumidityPct = clampedHumidity;
}

static void toggleRelayFromButton() {
  setSwitchState(!cha_switch_on.value.bool_value, true);
}

static void readSensors() {
  const uint32_t now = millis();
  if (now - lastSensorReadMs < SENSOR_READ_INTERVAL_MS) {
    return;
  }
  lastSensorReadMs = now;

  TempAndHumidity climate = dht.getTempAndHumidity();
  if (!isnan(climate.temperature) && !isnan(climate.humidity)) {
    const float adjustedTemperature = climate.temperature + (sensorConfig.temperature_offset_tenths / 10.0f);
    const float adjustedHumidity = climate.humidity + (sensorConfig.humidity_offset_tenths / 10.0f);
    updateClimateState(adjustedTemperature, adjustedHumidity, true);
  }

  updateLightState(analogRead(LDR_PIN), true);
}

static void handlePirInput() {
  if (!sensorConfig.pir_enabled) {
    if (pirMotionState) {
      updateMotionState(false, true);
    }
    return;
  }

  const uint32_t now = millis();
  const bool pirSignal = digitalRead(PIR_PIN) == HIGH;

  if (pirSignal) {
    pirLastMotionMs = now;
    if (!pirMotionState && now - pirLastTransitionMs >= sensorConfig.pir_cooldown_ms) {
      pirLastTransitionMs = now;
      updateMotionState(true, true);
    }
    return;
  }

  if (pirMotionState && pirLastMotionMs != 0 && now - pirLastMotionMs >= sensorConfig.pir_hold_ms) {
    pirLastTransitionMs = now;
    updateMotionState(false, true);
  }
}

static void handleButtonInput() {
  const bool pressedNow = digitalRead(BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  if (pressedNow && !buttonPressed && now - lastButtonToggleMs >= sensorConfig.button_debounce_ms) {
    buttonPressed = true;
    lastButtonToggleMs = now;
    logf("Buton tetiklendi.");
    toggleRelayFromButton();
  } else if (!pressedNow) {
    buttonPressed = false;
  }
}

static String buildStatusJson() {
  String body;
  body.reserve(3072);
  body += "{";
  body += "\"device\":{";
  body += "\"name\":\"" + String(deviceName) + "\",";
  body += "\"model\":\"D1MiniRelay\",";
  body += "\"firmware\":\"" APP_VERSION "\",";
  body += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\",";
  body += "\"flash_chip_id\":\"" + String(ESP.getFlashChipId(), HEX) + "\",";
  body += "\"cpu_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  body += "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"";
  body += "},";
  body += "\"network\":{";
  body += "\"hostname\":\"" + String(otaHostname) + "\",";
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
  body += "\"gpio\":" + String(RELAY_PIN) + ",";
  body += "\"brightness\":" + String(cha_switch_brightness.value.int_value);
  body += "},";
  body += "\"outputs\":{";
  body += "\"aux1\":{\"gpio\":\"D7\",\"on\":";
  body += cha_aux1_on.value.bool_value ? "true" : "false";
  body += ",\"brightness\":" + String(cha_aux1_brightness.value.int_value) + ",\"inverted\":";
  body += sensorConfig.aux1_inverted ? "true" : "false";
  body += "},";
  body += "\"aux2\":{\"gpio\":\"D8\",\"on\":";
  body += cha_aux2_on.value.bool_value ? "true" : "false";
  body += ",\"brightness\":" + String(cha_aux2_brightness.value.int_value) + ",\"inverted\":";
  body += sensorConfig.aux2_inverted ? "true" : "false";
  body += "}";
  body += "},";
  body += "\"sensors\":{";
  body += "\"temperature_c\":" + String(lastTemperatureC, 1) + ",";
  body += "\"humidity_pct\":" + String(lastHumidityPct, 1) + ",";
  body += "\"motion\":";
  body += pirMotionState ? "true" : "false";
  body += ",";
  body += "\"ldr_raw\":" + String(lastLdrRaw) + ",";
  body += "\"ldr_dark\":";
  body += ldrDarkState ? "true" : "false";
  body += ",";
  body += "\"light_lux\":" + String(lastLux, 1);
  body += "},";
  body += "\"config\":{";
  body += "\"pins\":{\"relay\":\"D1\",\"button\":\"D2\",\"pir\":\"D5\",\"dht\":\"D6\",\"ldr\":\"A0\",\"aux1\":\"D7\",\"aux2\":\"D8\"},";
  body += "\"pir_enabled\":";
  body += sensorConfig.pir_enabled ? "true" : "false";
  body += ",";
  body += "\"ldr_threshold\":" + String(sensorConfig.ldr_threshold) + ",";
  body += "\"ldr_hysteresis\":" + String(sensorConfig.ldr_hysteresis) + ",";
  body += "\"pir_hold_ms\":" + String(sensorConfig.pir_hold_ms) + ",";
  body += "\"pir_hold_seconds\":" + String(sensorConfig.pir_hold_ms / 1000.0f, 1) + ",";
  body += "\"pir_cooldown_ms\":" + String(sensorConfig.pir_cooldown_ms) + ",";
  body += "\"temperature_offset_c\":" + String(sensorConfig.temperature_offset_tenths / 10.0f, 1) + ",";
  body += "\"humidity_offset_pct\":" + String(sensorConfig.humidity_offset_tenths / 10.0f, 1) + ",";
  body += "\"button_debounce_ms\":" + String(sensorConfig.button_debounce_ms) + ",";
  body += "\"aux1_inverted\":";
  body += sensorConfig.aux1_inverted ? "true" : "false";
  body += ",";
  body += "\"aux2_inverted\":";
  body += sensorConfig.aux2_inverted ? "true" : "false";
  body += ",";
  body += "\"aux1_default_brightness\":" + String(sensorConfig.aux1_default_brightness) + ",";
  body += "\"aux2_default_brightness\":" + String(sensorConfig.aux2_default_brightness);
  body += "},";
  body += "\"homekit\":{";
  body += "\"clients\":" + String(arduino_homekit_connected_clients_count()) + ",";
  body += "\"pairing_code\":\"111-11-111\",";
  body += "\"ever_had_client\":";
  body += homekitEverHadClient ? "true" : "false";
  body += ",";
  body += "\"last_client_seen_ms_ago\":" + String(homekitLastClientSeenMs == 0 ? 0 : millis() - homekitLastClientSeenMs) + ",";
  body += "\"recovery_idle_ms\":" + String(HOMEKIT_RECOVERY_IDLE_MS) + ",";
  body += "\"recovery_grace_ms\":" + String(HOMEKIT_RECOVERY_GRACE_MS) + ",";
  body += "\"recovery_cooldown_ms\":" + String(HOMEKIT_RECOVERY_COOLDOWN_MS) + ",";
  body += "\"recovery_pending\":";
  body += homekitRecoveryPending ? "true" : "false";
  body += "},";
  body += "\"update\":{";
  body += "\"auto_update_enabled\":";
  body += AUTO_UPDATE_ENABLED ? "true" : "false";
  body += ",";
  body += "\"current_version\":\"" APP_VERSION "\",";
  body += "\"last_remote_version\":\"" + jsonEscape(lastRemoteVersion) + "\",";
  body += "\"last_remote_bin_url\":\"" + jsonEscape(lastRemoteBinUrl) + "\",";
  body += "\"last_remote_build_time\":\"" + jsonEscape(lastRemoteBuildTime) + "\",";
  body += "\"last_remote_commit\":\"" + jsonEscape(lastRemoteCommit) + "\",";
  body += "\"last_remote_notes\":\"" + jsonEscape(lastRemoteNotes) + "\",";
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
  body += "\"config\":\"http://" + WiFi.localIP().toString() + "/config\",";
  body += "\"setup\":\"";
  body += (WiFi.status() == WL_CONNECTED) ? "http://" + WiFi.localIP().toString() + "/setup" : "http://192.168.4.1/setup";
  body += "\",";
  body += "\"telnet\":\"telnet " + WiFi.localIP().toString() + " 23\",";
  body += "\"ota_host\":\"" + String(otaHostname) + ".local\"";
  body += "}";
  body += "}";
  return body;
}

static bool refreshUpdateMetadata(bool forceCheck) {
  if (WiFi.status() != WL_CONNECTED || updateInProgress) {
    return false;
  }

  const uint32_t now = millis();
  if (!forceCheck && lastUpdateCheckMs != 0 && now - lastUpdateCheckMs < UPDATE_CHECK_INTERVAL_MS) {
    return true;
  }
  lastUpdateCheckMs = now;

  String payload;
  String requestError;
  if (!httpsGetString(UPDATE_MANIFEST_URL, payload, requestError)) {
    lastUpdateResult = "manifest_" + requestError;
    return false;
  }
  lastSuccessfulUpdateCheckMs = now;

  lastRemoteVersion = extractJsonString(payload, "version");
  lastRemoteBinUrl = extractJsonString(payload, "bin_url");
  lastRemoteBuildTime = extractJsonString(payload, "built_at");
  lastRemoteCommit = extractJsonString(payload, "commit");
  lastRemoteNotes = extractJsonString(payload, "notes");
  if (lastRemoteNotes.length() == 0) {
    lastRemoteNotes = extractJsonValue(payload, "notes");
  }

  if (lastRemoteVersion.length() == 0 || lastRemoteBinUrl.length() == 0) {
    lastUpdateResult = "manifest_parse_failed";
    return false;
  }

  if (lastRemoteVersion == APP_VERSION) {
    updateAvailable = false;
    lastUpdateResult = "up_to_date";
    return true;
  }

  updateAvailable = true;
  lastUpdateResult = "update_available";
  return true;
}

static bool installPendingUpdate() {
  if (WiFi.status() != WL_CONNECTED || updateInProgress) {
    return false;
  }

  if (!refreshUpdateMetadata(true)) {
    return false;
  }

  if (!updateAvailable || lastRemoteBinUrl.length() == 0) {
    return false;
  }

  updateInProgress = true;
  lastUpdateResult = "updating";
  logf("Manuel update baslatildi. Hedef surum: %s", lastRemoteVersion.c_str());

  String updateError;
  if (httpsUpdateFromUrl(lastRemoteBinUrl, updateError)) {
    lastUpdateResult = "update_ok";
    logf("Manuel update tamamlandi. Yeniden baslatiliyor.");
    delay(500);
    ESP.restart();
    return true;
  }

  updateInProgress = false;
  lastUpdateResult = "update_" + updateError;
  logf("Manuel update hatasi: %s", updateError.c_str());
  return false;
}

static void setSwitchState(bool on, bool notifyHomeKit) {
  cha_switch_on.value.bool_value = on;
  cha_switch_brightness.value.int_value = on ? (cha_switch_brightness.value.int_value > 0 ? cha_switch_brightness.value.int_value : 100) : 0;
  setRelay(on);
  logf("Role durumu: %s", on ? "ACIK" : "KAPALI");

  if (notifyHomeKit) {
    homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
    homekit_characteristic_notify(&cha_switch_brightness, cha_switch_brightness.value);
  }
}

static void setMainBrightness(int value, bool notifyHomeKit) {
  const int brightness = value < 0 ? 0 : (value > 100 ? 100 : value);
  cha_switch_brightness.value.int_value = brightness;
  setSwitchState(brightness > 0, notifyHomeKit);
}

static bool connectWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.hostname(otaHostname);
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

void switchBrightnessSetter(const homekit_value_t value) {
  setMainBrightness(value.int_value, false);
}

void aux1SwitchSetter(const homekit_value_t value) {
  const int brightness = cha_aux1_brightness.value.int_value > 0 ? cha_aux1_brightness.value.int_value : sensorConfig.aux1_default_brightness;
  applyAuxOutput(1, value.bool_value, brightness, false);
}

void aux1BrightnessSetter(const homekit_value_t value) {
  const int brightness = value.int_value;
  applyAuxOutput(1, brightness > 0, brightness, false);
}

void aux2SwitchSetter(const homekit_value_t value) {
  const int brightness = cha_aux2_brightness.value.int_value > 0 ? cha_aux2_brightness.value.int_value : sensorConfig.aux2_default_brightness;
  applyAuxOutput(2, value.bool_value, brightness, false);
}

void aux2BrightnessSetter(const homekit_value_t value) {
  const int brightness = value.int_value;
  applyAuxOutput(2, brightness > 0, brightness, false);
}

static void setupHomeKit() {
  cha_switch_on.setter = switchSetter;
  cha_switch_brightness.setter = switchBrightnessSetter;
  cha_aux1_on.setter = aux1SwitchSetter;
  cha_aux1_brightness.setter = aux1BrightnessSetter;
  cha_aux2_on.setter = aux2SwitchSetter;
  cha_aux2_brightness.setter = aux2BrightnessSetter;
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
  body += setupApName;
  body += "<br><small>Sifre: ";
  body += WIFI_SETUP_PASSWORD;
  body += "</small><br><small>Bu sayfa cihaz mevcut ağa bağlanamadığında açılır.</small></section>";
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

static void handleConfigPage() {
  String body;
  body.reserve(5000);
  body += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>Wemos Config</title><style>";
  body += "body{font-family:Georgia,serif;max-width:820px;margin:32px auto;padding:0 16px;background:#f3efe7;color:#171512}";
  body += "form,section{background:#fffdf8;padding:18px;border-radius:16px;border:1px solid #ddd5c7;box-shadow:0 10px 28px rgba(0,0,0,.05);margin-bottom:16px}";
  body += "label{display:block;font-size:14px;margin-top:10px} input,button{width:100%;padding:12px;margin-top:6px;border-radius:10px;border:1px solid #cfc6b6;font-size:16px}";
  body += "button{background:#171512;color:#fff;border:none} .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}";
  body += "small{color:#6a635a}";
  body += "</style></head><body>";
  body += "<h1>/config</h1>";
  body += "<section><strong>Pinler</strong><br><small>Role=D1, Buton=D2, PIR=D5, DHT=D6, LDR=A0, PWM1=D7, PWM2=D8</small></section>";
  body += "<form method='POST' action='/config'>";
  body += "<div class='grid'>";
  body += "<label>LDR esik (0-1023)<input name='ldr_threshold' type='number' min='0' max='1023' value='" + String(sensorConfig.ldr_threshold) + "'></label>";
  body += "<label>LDR histerezis<input name='ldr_hysteresis' type='number' min='0' max='400' value='" + String(sensorConfig.ldr_hysteresis) + "'></label>";
  body += "<label>PIR aktif 0/1<input name='pir_enabled' type='number' min='0' max='1' value='" + String(sensorConfig.pir_enabled ? 1 : 0) + "'></label>";
  body += "<label>PIR aktif tutma sn<input name='pir_hold_seconds' type='number' min='1' max='120' step='1' value='" + String(sensorConfig.pir_hold_ms / 1000) + "'></label>";
  body += "<label>PIR cooldown ms<input name='pir_cooldown_ms' type='number' min='100' max='30000' value='" + String(sensorConfig.pir_cooldown_ms) + "'></label>";
  body += "<label>Sicaklik offset (0.1C)<input name='temperature_offset_tenths' type='number' min='-100' max='100' value='" + String(sensorConfig.temperature_offset_tenths) + "'></label>";
  body += "<label>Nem offset (0.1%)<input name='humidity_offset_tenths' type='number' min='-100' max='100' value='" + String(sensorConfig.humidity_offset_tenths) + "'></label>";
  body += "<label>Buton debounce ms<input name='button_debounce_ms' type='number' min='50' max='2000' value='" + String(sensorConfig.button_debounce_ms) + "'></label>";
  body += "<label>PWM1 varsayilan parlaklik<input name='aux1_default_brightness' type='number' min='0' max='100' value='" + String(sensorConfig.aux1_default_brightness) + "'></label>";
  body += "<label>PWM2 varsayilan parlaklik<input name='aux2_default_brightness' type='number' min='0' max='100' value='" + String(sensorConfig.aux2_default_brightness) + "'></label>";
  body += "<label>PWM1 invert 0/1<input name='aux1_inverted' type='number' min='0' max='1' value='" + String(sensorConfig.aux1_inverted ? 1 : 0) + "'></label>";
  body += "<label>PWM2 invert 0/1<input name='aux2_inverted' type='number' min='0' max='1' value='" + String(sensorConfig.aux2_inverted ? 1 : 0) + "'></label>";
  body += "</div><button type='submit'>Kaydet</button></form>";
  body += "</body></html>";
  webServer.send(200, "text/html; charset=utf-8", body);
}

static void handleConfigSave() {
  sensorConfig.ldr_threshold = clampU16(webServer.arg("ldr_threshold").toInt(), 0, 1023);
  sensorConfig.ldr_hysteresis = clampU16(webServer.arg("ldr_hysteresis").toInt(), 0, 400);
  sensorConfig.pir_enabled = webServer.arg("pir_enabled").toInt() == 1;
  sensorConfig.pir_hold_ms = clampU32(webServer.arg("pir_hold_seconds").toInt() * 1000UL, 1000, 120000);
  sensorConfig.pir_cooldown_ms = clampU16(webServer.arg("pir_cooldown_ms").toInt(), 100, 30000);
  sensorConfig.temperature_offset_tenths = clampI8(webServer.arg("temperature_offset_tenths").toInt(), -100, 100);
  sensorConfig.humidity_offset_tenths = clampI8(webServer.arg("humidity_offset_tenths").toInt(), -100, 100);
  sensorConfig.button_debounce_ms = clampU16(webServer.arg("button_debounce_ms").toInt(), 50, 2000);
  sensorConfig.aux1_default_brightness = clampU8(webServer.arg("aux1_default_brightness").toInt(), 0, 100);
  sensorConfig.aux2_default_brightness = clampU8(webServer.arg("aux2_default_brightness").toInt(), 0, 100);
  sensorConfig.aux1_inverted = webServer.arg("aux1_inverted").toInt() == 1;
  sensorConfig.aux2_inverted = webServer.arg("aux2_inverted").toInt() == 1;

  if (!saveSensorConfig()) {
    webServer.send(500, "text/plain", "Config kaydedilemedi.");
    return;
  }

  updateLightState(analogRead(LDR_PIN), false);
  applyAuxOutput(1, cha_aux1_on.value.bool_value, cha_aux1_brightness.value.int_value, false);
  applyAuxOutput(2, cha_aux2_on.value.bool_value, cha_aux2_brightness.value.int_value, false);
  webServer.send(200, "text/html; charset=utf-8",
                 "<html><body><h1>Kaydedildi</h1><p>Ayarlar uygulandi.</p><p><a href='/config'>Geri don</a></p></body></html>");
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
  body += "<div class='hero'><div><div class='sub'>Canli cihaz paneli</div><h1>" + String(deviceName) + "</h1></div><div class='pill'>Otomatik yenileme: 3 sn</div></div>";
  body += "<div class='grid'>";
  body += "<div class='card'><div class='label'>Role</div><div id='relay' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Wi-Fi</div><div id='wifi' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>RSSI</div><div id='rssi' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Heap</div><div id='heap' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Sicaklik</div><div id='temp' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Nem</div><div id='hum' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>PIR</div><div id='pir' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>Isik</div><div id='ldr' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>PWM1</div><div id='aux1' class='value'>-</div></div>";
  body += "<div class='card'><div class='label'>PWM2</div><div id='aux2' class='value'>-</div></div>";
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
  body += "set('temp',`${d.sensors.temperature_c} C`,d.sensors.temperature_c<35?'good':'warn');";
  body += "set('hum',`${d.sensors.humidity_pct} %`,d.sensors.humidity_pct<75?'good':'warn');";
  body += "set('pir',d.sensors.motion?'HAREKET':'BOS',d.sensors.motion?'warn':'good');";
  body += "set('ldr',`${d.sensors.light_lux} lux`,d.sensors.ldr_dark?'warn':'good');";
  body += "set('aux1',`${d.outputs.aux1.brightness}%`,d.outputs.aux1.on?'good':'warn');";
  body += "set('aux2',`${d.outputs.aux2.brightness}%`,d.outputs.aux2.on?'good':'warn');";
  body += "set('version',d.update.current_version,'mono');";
  body += "set('update',d.update.in_progress?'YUKLENIYOR':d.update.last_result,'mono');";
  body += "set('nextCheck',`${Math.ceil(d.update.next_check_in_ms/1000)} sn`,'mono');";
  body += "rows('networkRows',[['SSID',d.network.wifi_ssid],['IP',d.network.ip],['BSSID',d.network.bssid],['Kanal',d.network.channel],['RSSI',`${d.network.rssi} dBm`],['Reconnect',d.network.reconnect_count],['Retry',d.network.retry_count],['Setup AP',d.network.setup_ap_active?'ACIK':'KAPALI']]);";
  body += "rows('systemRows',[['Uptime(ms)',d.system.uptime_ms],['Reset',d.system.reset_reason],['Fragmentation',d.system.heap_fragmentation],['Max block',d.system.max_free_block],['Sketch',d.system.sketch_size],['Clients',d.homekit.clients],['Son istemci',`${Math.floor(d.homekit.last_client_seen_ms_ago/1000)} sn once`],['Recovery',d.homekit.recovery_pending?'BEKLIYOR':'NORMAL'],['LDR raw',d.sensors.ldr_raw],['Karanlik',d.sensors.ldr_dark?'EVET':'HAYIR']]);";
  body += "rows('serviceRows',[['Setup',d.services.setup],['HTTP',d.services.http],['Status',d.services.status],['Config',d.services.config],['PWM1',`${d.outputs.aux1.gpio} / ${d.outputs.aux1.inverted?'INV':'NOR'}`],['PWM2',`${d.outputs.aux2.gpio} / ${d.outputs.aux2.inverted?'INV':'NOR'}`],['Telnet',d.services.telnet],['OTA Host',d.services.ota_host],['Manifest',d.update.manifest_url],['Yeni surum',d.update.last_remote_version||'-']]);";
  body += "}catch(e){set('update','baglanti hatasi','warn');}} load(); setInterval(load," + String(STATUS_REFRESH_MS) + ");";
  body += "</script></body></html>";
  webServer.send(200, "text/html; charset=utf-8", body);
}

static void handleUpdatePage() {
  refreshUpdateMetadata(true);

  String notes = lastRemoteNotes;
  notes.replace("\\n", "\n");

  String body;
  body.reserve(6000);
  body += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>Wemos Update</title><style>";
  body += ":root{--bg:#f2efe8;--card:#fffdf8;--ink:#171512;--muted:#6a635a;--line:#ddd5c7;--accent:#0d6b5f;--warn:#9a3412}";
  body += "body{margin:0;background:linear-gradient(180deg,#f9f5ec 0,#efe9dd 100%);color:var(--ink);font-family:Georgia,serif}";
  body += ".wrap{max-width:920px;margin:0 auto;padding:24px}.hero{display:flex;justify-content:space-between;gap:16px;align-items:end}";
  body += ".card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px;box-shadow:0 10px 28px rgba(0,0,0,.06);margin-top:16px}";
  body += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}.label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}";
  body += ".value{font-size:26px;margin-top:6px}.mono{font-family:ui-monospace,monospace;font-size:14px}.ok{color:var(--accent)}.warn{color:var(--warn)}";
  body += "button{border:none;border-radius:999px;padding:12px 18px;font-size:15px;background:var(--ink);color:#fff;cursor:pointer}button.alt{background:#e8e0d0;color:var(--ink)}";
  body += "button:disabled{opacity:.5;cursor:not-allowed}.row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-top:1px solid var(--line)}.row:first-child{border-top:none}";
  body += "pre{white-space:pre-wrap;background:#f6f0e4;border-radius:12px;padding:14px;border:1px solid var(--line)}a{color:var(--accent);text-decoration:none}";
  body += "</style></head><body><div class='wrap'>";
  body += "<div class='hero'><div><div class='label'>Manuel Firmware Guncelleme</div><h1 style='margin:6px 0 0'>/update</h1></div><div class='mono'>Cihaz: " + String(deviceName) + "</div></div>";
  body += "<div class='grid'>";
  body += "<div class='card'><div class='label'>Mevcut Surum</div><div class='value mono'>" APP_VERSION "</div></div>";
  body += "<div class='card'><div class='label'>Son Bulunan Surum</div><div class='value mono'>";
  body += lastRemoteVersion.length() > 0 ? htmlEscape(lastRemoteVersion) : "-";
  body += "</div></div>";
  body += "<div class='card'><div class='label'>Durum</div><div id='updateState' class='value mono ";
  body += updateAvailable ? "ok" : "warn";
  body += "'>";
  body += htmlEscape(lastUpdateResult);
  body += "</div></div>";
  body += "</div>";
  body += "<div class='card'><div style='display:flex;gap:12px;flex-wrap:wrap'>";
  body += "<button id='checkBtn' class='alt' onclick='runAction(\"check\")'>Yenilikleri Kontrol Et</button>";
  body += "<button id='installBtn' onclick='runAction(\"install\")'";
  if (!updateAvailable || updateInProgress) {
    body += " disabled";
  }
  body += ">Surumu Yukle</button></div>";
  body += "<div id='flashMsg' class='mono' style='margin-top:12px;color:var(--muted)'>Yukleme sirasinda cihaz yeniden baslayabilir.</div></div>";
  body += "<div class='card'><div class='label'>Yenilikler</div><pre id='notes'>";
  if (notes.length() > 0) {
    body += htmlEscape(notes);
  } else {
    body += "Manifest notu yok. Commit: " + htmlEscape(lastRemoteCommit) + "\nBuild: " + htmlEscape(lastRemoteBuildTime);
  }
  body += "</pre></div>";
  body += "<div class='card'><div class='label'>Detay</div>";
  body += "<div class='row'><span>Commit</span><span class='mono' id='commit'>" + htmlEscape(lastRemoteCommit) + "</span></div>";
  body += "<div class='row'><span>Build Time</span><span class='mono' id='builtAt'>" + htmlEscape(lastRemoteBuildTime) + "</span></div>";
  body += "<div class='row'><span>Manifest</span><span class='mono'><a href='" UPDATE_MANIFEST_URL "'>" UPDATE_MANIFEST_URL "</a></span></div>";
  body += "<div class='row'><span>Firmware</span><span class='mono' id='binUrl'>" + htmlEscape(lastRemoteBinUrl) + "</span></div>";
  body += "</div>";
  body += "</div><script>";
  body += "async function load(){const r=await fetch('/api/status',{cache:'no-store'});return r.json();}";
  body += "function sync(d){document.getElementById('updateState').textContent=d.update.last_result;";
  body += "document.getElementById('notes').textContent=d.update.last_remote_notes||(`Manifest notu yok. Commit: ${d.update.last_remote_commit}\\nBuild: ${d.update.last_remote_build_time}`);";
  body += "document.getElementById('commit').textContent=d.update.last_remote_commit||'-';";
  body += "document.getElementById('builtAt').textContent=d.update.last_remote_build_time||'-';";
  body += "document.getElementById('binUrl').textContent=d.update.last_remote_bin_url||'-';";
  body += "document.getElementById('installBtn').disabled=!d.update.available||d.update.in_progress;}";
  body += "async function runAction(kind){const msg=document.getElementById('flashMsg');msg.textContent='Islem baslatiliyor...';";
  body += "const res=await fetch(kind==='check'?'/api/update/check':'/api/update/install',{method:'POST'});const data=await res.json();";
  body += "msg.textContent=data.message||data.result||'Tamam';const state=await load();sync(state);} load().then(sync); setInterval(()=>load().then(sync).catch(()=>{}),5000);";
  body += "</script></body></html>";
  webServer.send(200, "text/html; charset=utf-8", body);
}

static void handleUpdateCheck() {
  const bool ok = refreshUpdateMetadata(true);
  String body = "{\"ok\":";
  body += ok ? "true" : "false";
  body += ",\"result\":\"" + jsonEscape(lastUpdateResult) + "\",\"message\":\"";
  body += ok ? "Guncelleme bilgisi yenilendi." : "Manifest okunamadi.";
  body += "\"}";
  webServer.send(ok ? 200 : 500, "application/json", body);
}

static void handleUpdateInstall() {
  if (updateInProgress) {
    webServer.send(409, "application/json", "{\"ok\":false,\"result\":\"busy\",\"message\":\"Guncelleme zaten suruyor.\"}");
    return;
  }

  const bool started = installPendingUpdate();
  String body = "{\"ok\":";
  body += started ? "true" : "false";
  body += ",\"result\":\"" + jsonEscape(lastUpdateResult) + "\",\"message\":\"";
  body += started ? "Yukleme baslatildi. Cihaz yeniden baslayabilir." : "Yukleme baslatilamadi.";
  body += "\"}";
  webServer.send(started ? 200 : 500, "application/json", body);
}

static void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/api/status", handleApiStatus);
  webServer.on("/api/update/check", HTTP_POST, handleUpdateCheck);
  webServer.on("/api/update/install", HTTP_POST, handleUpdateInstall);
  webServer.on("/status", handleStatusPage);
  webServer.on("/update", HTTP_GET, handleUpdatePage);
  webServer.on("/config", HTTP_GET, handleConfigPage);
  webServer.on("/config", HTTP_POST, handleConfigSave);
  webServer.on("/setup", HTTP_GET, handleSetupPage);
  webServer.on("/setup", HTTP_POST, handleSetupSave);
  webServer.begin();
  logf("HTTP hazir: http://%s/", WiFi.localIP().toString().c_str());
}

static void setupOta() {
  ArduinoOTA.setHostname(otaHostname);
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
  logf("OTA hazir: %s.local", otaHostname);
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
  if (!AUTO_UPDATE_ENABLED) {
    if (lastUpdateResult == "never_checked") {
      lastUpdateResult = "disabled";
    }
    return;
  }

  refreshUpdateMetadata(false);
}

static void handleHomeKitHealth() {
  if (WiFi.status() != WL_CONNECTED || updateInProgress) {
    return;
  }

  const uint32_t now = millis();
  const int clientCount = arduino_homekit_connected_clients_count();

  if (clientCount > 0) {
    homekitEverHadClient = true;
    homekitLastClientSeenMs = now;
    homekitRecoveryPending = false;
    return;
  }

  if (!homekitEverHadClient) {
    return;
  }

  if (now < HOMEKIT_RECOVERY_GRACE_MS) {
    return;
  }

  if (homekitLastClientSeenMs == 0) {
    homekitLastClientSeenMs = now;
    return;
  }

  if (now - homekitLastClientSeenMs < HOMEKIT_RECOVERY_IDLE_MS) {
    return;
  }

  if (homekitLastRecoveryRestartMs != 0 && now - homekitLastRecoveryRestartMs < HOMEKIT_RECOVERY_COOLDOWN_MS) {
    return;
  }

  if (!homekitRecoveryPending) {
    homekitRecoveryPending = true;
    logf("HomeKit istemcisi %lu ms yok. Servis toparlama icin yeniden baslatiliyor.", now - homekitLastClientSeenMs);
  }

  homekitLastRecoveryRestartMs = now;
  delay(200);
  ESP.restart();
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
      homekitLastClientSeenMs = now;
      homekitRecoveryPending = false;
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
    homekitRecoveryPending = false;
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
  prepareDeviceIdentity();
  logf("D1 Mini HomeKit role baslatiliyor...");

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT);
  pinMode(AUX1_PWM_PIN, OUTPUT);
  pinMode(AUX2_PWM_PIN, OUTPUT);
  analogWriteRange(1023);
  setRelay(false);
  dht.setup(DHT_PIN, DHTesp::DHT11);

  loadWifiCredentials();
  loadSensorConfig();
  updateClimateState(lastTemperatureC, lastHumidityPct, false);
  updateLightState(analogRead(LDR_PIN), false);
  updateMotionState(false, false);
  cha_switch_brightness.value.int_value = 100;
  applyAuxOutput(1, false, sensorConfig.aux1_default_brightness, false);
  applyAuxOutput(2, false, sensorConfig.aux2_default_brightness, false);
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
  handleButtonInput();
  handlePirInput();
  readSensors();
  handleTelnet();
  webServer.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    checkForUpdates();
    ArduinoOTA.handle();
    arduino_homekit_loop();
    handleHomeKitHealth();
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
