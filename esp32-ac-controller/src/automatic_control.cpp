#include "automatic_control.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_arduino_version.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <ctype.h>
#include <math.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
constexpr bool kWifiSecretsHeaderPresent = true;
#else
constexpr char kWifiSsid[] = "";
constexpr char kWifiPassword[] = "";
constexpr bool kWifiSecretsHeaderPresent = false;
#endif

namespace {

constexpr uint32_t kWifiRetryIntervalMs = 30000;
constexpr char kKoreanTimezone[] = "KST-9";
constexpr char kPreferencesNamespace[] = "ac-auto";
constexpr char kDefaultAutomaticProfile[] =
    "cool_27_f1_swing_on_turbo_off";
constexpr char kCredentialFingerprintPrefix[] =
    "esp32-ac-controller:wifi:v1";
constexpr uint32_t kWifiDiagnosticTimeoutMs = 15000;
constexpr uint16_t kWifiDiagnosticMaxAccessPoints = 8;
// Some ESP32-C3 SuperMini revisions have an RF layout issue at higher output.
// 8.5 dBm is the highest level reported stable on the affected boards.
constexpr wifi_power_t kWifiTransmitPower = WIFI_POWER_8_5dBm;

AutomaticControlSettings automaticSettings = {
    true, 6, 0, 28.0F, AutomaticTriggerMode::kTimeAndTemperature};
char automaticOnProfileLabel[32] = "cool_27_f1_swing_on_turbo_off";
bool wifiConfigured = false;
bool ntpConfigured = false;
int32_t lastSentDayKey = -1;
bool temperatureTriggerLatched = false;
uint32_t lastAutomaticAttemptMs = 0;
uint32_t lastWifiAttemptMs = 0;
char statusText[16] = "DISABLED";
bool wifiDiagnosticActive = false;
uint32_t wifiDiagnosticStartedMs = 0;
uint16_t wifiDiagnosticDisconnectCount = 0;
uint8_t wifiDiagnosticLastReason = 0;
int8_t wifiDiagnosticLastRssi = 0;

const char *wifiAuthName(wifi_auth_mode_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI-PSK";
    default:
      return "UNKNOWN";
  }
}

const char *wifiCipherName(wifi_cipher_type_t cipher) {
  switch (cipher) {
    case WIFI_CIPHER_TYPE_NONE:
      return "NONE";
    case WIFI_CIPHER_TYPE_WEP40:
      return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:
      return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:
      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:
      return "CCMP(AES)";
    case WIFI_CIPHER_TYPE_TKIP_CCMP:
      return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128:
      return "AES-CMAC128";
    case WIFI_CIPHER_TYPE_SMS4:
      return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP:
      return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256:
      return "GCMP256";
    case WIFI_CIPHER_TYPE_AES_GMAC128:
      return "AES-GMAC128";
    case WIFI_CIPHER_TYPE_AES_GMAC256:
      return "AES-GMAC256";
    default:
      return "UNKNOWN";
  }
}

void printMacAddress(const uint8_t *address) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1],
                address[2], address[3], address[4], address[5]);
}

uint32_t wifiEventElapsedMs() {
  return wifiDiagnosticActive ? millis() - wifiDiagnosticStartedMs : 0;
}

void onWifiConnected(arduino_event_id_t, arduino_event_info_t info) {
  Serial.print("Wi-Fi associated: BSSID=");
  printMacAddress(info.wifi_sta_connected.bssid);
  Serial.printf(" channel=%u auth=%s(%d)", info.wifi_sta_connected.channel,
                wifiAuthName(info.wifi_sta_connected.authmode),
                static_cast<int>(info.wifi_sta_connected.authmode));
  if (wifiDiagnosticActive) {
    Serial.printf(" elapsed=%lu ms",
                  static_cast<unsigned long>(wifiEventElapsedMs()));
  }
  Serial.println();
}

void onWifiGotIp(arduino_event_id_t, arduino_event_info_t) {
  Serial.printf("Wi-Fi got IP: %s", WiFi.localIP().toString().c_str());
  if (wifiDiagnosticActive) {
    Serial.printf(" elapsed=%lu ms",
                  static_cast<unsigned long>(wifiEventElapsedMs()));
  }
  Serial.println();
}

void onWifiDisconnected(arduino_event_id_t, arduino_event_info_t info) {
  const wifi_event_sta_disconnected_t &event = info.wifi_sta_disconnected;
  if (wifiDiagnosticActive) {
    ++wifiDiagnosticDisconnectCount;
    wifiDiagnosticLastReason = event.reason;
    wifiDiagnosticLastRssi = event.rssi;
  }

  Serial.printf("Wi-Fi disconnected: reason=%u (%s), BSSID=", event.reason,
                WiFi.disconnectReasonName(
                    static_cast<wifi_err_reason_t>(event.reason)));
  printMacAddress(event.bssid);
  Serial.printf(", RSSI=%d dBm", event.rssi);
  if (wifiDiagnosticActive) {
    Serial.printf(", elapsed=%lu ms",
                  static_cast<unsigned long>(wifiEventElapsedMs()));
  }
  Serial.println();
}

void printEspError(const char *operation, esp_err_t result) {
  Serial.printf("  %-22s: %s (0x%X)\n", operation, esp_err_to_name(result),
                static_cast<unsigned int>(result));
}

void printAccessPointRecord(const wifi_ap_record_t &record, uint16_t index) {
  Serial.printf("Target AP #%u\n", index + 1);
  Serial.printf("  SSID                  : %s\n", record.ssid);
  Serial.print("  BSSID                 : ");
  printMacAddress(record.bssid);
  Serial.println();
  Serial.printf("  signal/channel        : %d dBm / %u\n", record.rssi,
                record.primary);
  Serial.printf("  channel width         : %s (secondary=%d)\n",
                record.second == WIFI_SECOND_CHAN_NONE ? "HT20" : "HT40",
                static_cast<int>(record.second));
  Serial.printf("  authentication        : %s (%d)\n",
                wifiAuthName(record.authmode),
                static_cast<int>(record.authmode));
  Serial.printf("  pairwise/group cipher : %s / %s\n",
                wifiCipherName(record.pairwise_cipher),
                wifiCipherName(record.group_cipher));
  Serial.printf("  PHY advertisement     : 11b=%u 11g=%u 11n=%u LR=%u\n",
                record.phy_11b, record.phy_11g, record.phy_11n,
                record.phy_lr);
  Serial.printf("  WPS/FTM responder     : %u / %u\n", record.wps,
                record.ftm_responder);
  Serial.printf("  country               : %.2s channels=%u..%u policy=%d\n",
                record.country.cc, record.country.schan,
                record.country.schan + record.country.nchan - 1,
                static_cast<int>(record.country.policy));
}

void printStationRadioConfiguration() {
  uint8_t stationMac[6] = {};
  uint8_t protocols = 0;
  wifi_bandwidth_t bandwidth = WIFI_BW_HT20;
  wifi_country_t country = {};
  wifi_ps_type_t powerSave = WIFI_PS_NONE;

  const esp_err_t macResult = esp_wifi_get_mac(WIFI_IF_STA, stationMac);
  const esp_err_t protocolResult =
      esp_wifi_get_protocol(WIFI_IF_STA, &protocols);
  const esp_err_t bandwidthResult =
      esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
  const esp_err_t countryResult = esp_wifi_get_country(&country);
  const esp_err_t powerSaveResult = esp_wifi_get_ps(&powerSave);

  if (macResult == ESP_OK) {
    Serial.print("  station MAC           : ");
    printMacAddress(stationMac);
    Serial.println();
  } else {
    printEspError("station MAC", macResult);
  }
  if (protocolResult == ESP_OK) {
    Serial.printf("  station protocols     : 11b=%u 11g=%u 11n=%u LR=%u (0x%02X)\n",
                  !!(protocols & WIFI_PROTOCOL_11B),
                  !!(protocols & WIFI_PROTOCOL_11G),
                  !!(protocols & WIFI_PROTOCOL_11N),
                  !!(protocols & WIFI_PROTOCOL_LR), protocols);
  } else {
    printEspError("station protocols", protocolResult);
  }
  if (bandwidthResult == ESP_OK) {
    Serial.printf("  station bandwidth     : %s\n",
                  bandwidth == WIFI_BW_HT20 ? "HT20" : "HT40");
  } else {
    printEspError("station bandwidth", bandwidthResult);
  }
  if (countryResult == ESP_OK) {
    Serial.printf("  station country       : %.2s channels=%u..%u policy=%d\n",
                  country.cc, country.schan,
                  country.schan + country.nchan - 1,
                  static_cast<int>(country.policy));
  } else {
    printEspError("station country", countryResult);
  }
  if (powerSaveResult == ESP_OK) {
    Serial.printf("  power save            : %d\n",
                  static_cast<int>(powerSave));
  } else {
    printEspError("power save", powerSaveResult);
  }
}

void printStationConnectionConfiguration() {
  wifi_config_t config = {};
  const esp_err_t result = esp_wifi_get_config(WIFI_IF_STA, &config);
  if (result != ESP_OK) {
    printEspError("station configuration", result);
    return;
  }

  Serial.printf("  fixed BSSID/channel   : %s / %u\n",
                config.sta.bssid_set ? "yes" : "no", config.sta.channel);
  if (config.sta.bssid_set) {
    Serial.print("  requested BSSID       : ");
    printMacAddress(config.sta.bssid);
    Serial.println();
  }
  Serial.printf("  scan/sort method      : %d / %d\n",
                static_cast<int>(config.sta.scan_method),
                static_cast<int>(config.sta.sort_method));
  Serial.printf("  threshold RSSI/auth   : %d / %s(%d)\n",
                config.sta.threshold.rssi,
                wifiAuthName(config.sta.threshold.authmode),
                static_cast<int>(config.sta.threshold.authmode));
  Serial.printf("  PMF capable/required  : %u / %u\n", config.sta.pmf_cfg.capable,
                config.sta.pmf_cfg.required);
  Serial.printf("  listen interval       : %u\n", config.sta.listen_interval);
}

void setStatus(const char *status) {
  snprintf(statusText, sizeof(statusText), "%s", status);
}

void startWifi(uint32_t nowMs) {
  lastWifiAttemptMs = nowMs;
  Serial.printf("Wi-Fi connection start: %s\n", kWifiSsid);
  WiFi.disconnect();
  WiFi.begin(kWifiSsid, kWifiPassword);
  setStatus("WIFI WAIT");
}

bool readKoreanTime(tm *localTime) {
  const time_t now = time(nullptr);
  if (now < 1700000000 || !localtime_r(&now, localTime)) {
    return false;
  }
  return true;
}

int32_t makeDayKey(const tm &localTime) {
  return (localTime.tm_year + 1900) * 1000 + localTime.tm_yday;
}

bool settingsAreValid(const AutomaticControlSettings &settings) {
  return settings.startHour <= 23 && settings.startMinute <= 59 &&
         settings.onTemperatureC >= 16.0F &&
         settings.onTemperatureC <= 35.0F &&
         static_cast<uint8_t>(settings.triggerMode) <=
             static_cast<uint8_t>(AutomaticTriggerMode::kTemperatureOnly);
}

bool profileLabelIsValid(const char *label) {
  if (!label || !label[0] || strlen(label) >= sizeof(automaticOnProfileLabel)) {
    return false;
  }
  for (const char *cursor = label; *cursor; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (!isalnum(value) && value != '_' && value != '-') {
      return false;
    }
  }
  return true;
}

void loadSettings() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    Serial.println("No saved automatic settings; using defaults.");
    return;
  }

  AutomaticControlSettings loaded = {
      preferences.getBool("enabled", automaticSettings.enabled),
      preferences.getUChar("hour", automaticSettings.startHour),
      preferences.getUChar("minute", automaticSettings.startMinute),
      preferences.getFloat("temp", automaticSettings.onTemperatureC),
      static_cast<AutomaticTriggerMode>(preferences.getUChar(
          "mode", static_cast<uint8_t>(automaticSettings.triggerMode)))};
  const String loadedProfile =
      preferences.getString("profile", kDefaultAutomaticProfile);
  preferences.end();
  if (settingsAreValid(loaded)) {
    automaticSettings = loaded;
  } else {
    Serial.println("Invalid automatic settings found; using defaults.");
  }
  if (profileLabelIsValid(loadedProfile.c_str())) {
    snprintf(automaticOnProfileLabel, sizeof(automaticOnProfileLabel), "%s",
             loadedProfile.c_str());
  } else {
    Serial.println("Invalid automatic profile found; using default.");
  }
}

}  // namespace

void setupAutomaticControl() {
  loadSettings();
  printAutomaticControlConfiguration();

  wifiConfigured = kWifiSsid[0] != '\0';
  if (!wifiConfigured) {
    Serial.println("Wi-Fi time features unavailable: create "
                   "include/wifi_secrets.h first.");
    setStatus("NO WIFI CFG");
    return;
  }

  WiFi.mode(WIFI_STA);
  const bool transmitPowerSet = WiFi.setTxPower(kWifiTransmitPower);
  Serial.printf("Wi-Fi TX power limit: 8.5 dBm (%s)\n",
                transmitPowerSet ? "OK" : "FAILED");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  startWifi(millis());
}

AutomaticControlCommand pollAutomaticControl(float temperatureC) {
  const uint32_t nowMs = millis();
  const bool temperatureOnly =
      automaticSettings.triggerMode == AutomaticTriggerMode::kTemperatureOnly;
  bool clockReady = false;
  tm localTime = {};

  if (wifiConfigured && WiFi.status() != WL_CONNECTED) {
    ntpConfigured = false;
    if (nowMs - lastWifiAttemptMs >= kWifiRetryIntervalMs) {
      startWifi(nowMs);
    }
  } else if (wifiConfigured) {
    if (!ntpConfigured) {
      configTzTime(kKoreanTimezone, "pool.ntp.org", "time.google.com",
                   "time.cloudflare.com");
      ntpConfigured = true;
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("NTP synchronization requested (Asia/Seoul).");
    }
    clockReady = readKoreanTime(&localTime);
  }

  if (!automaticSettings.enabled) {
    setStatus("AUTO OFF");
    return AutomaticControlCommand::kNone;
  }

  if (temperatureOnly) {
    if (isnan(temperatureC)) {
      setStatus("SENSOR ERR");
      return AutomaticControlCommand::kNone;
    }
    if (temperatureC <= automaticSettings.onTemperatureC - 0.5F) {
      temperatureTriggerLatched = false;
    }
    if (temperatureTriggerLatched) {
      setStatus("TEMP SENT");
      return AutomaticControlCommand::kNone;
    }
    if (temperatureC > automaticSettings.onTemperatureC) {
      if (nowMs - lastAutomaticAttemptMs < 30000 &&
          lastAutomaticAttemptMs != 0) {
        setStatus("ON READY");
        return AutomaticControlCommand::kNone;
      }
      lastAutomaticAttemptMs = nowMs;
      setStatus("ON READY");
      return AutomaticControlCommand::kSendOn;
    }
    setStatus("TEMP WAIT");
    return AutomaticControlCommand::kNone;
  }

  if (!wifiConfigured || WiFi.status() != WL_CONNECTED) {
    setStatus(wifiConfigured ? "WIFI WAIT" : "NO WIFI CFG");
    return AutomaticControlCommand::kNone;
  }
  if (!clockReady) {
    setStatus("TIME SYNC");
    return AutomaticControlCommand::kNone;
  }

  const int32_t todayKey = makeDayKey(localTime);
  if (lastSentDayKey == todayKey) {
    setStatus("ON SENT");
    return AutomaticControlCommand::kNone;
  }

  const uint16_t currentMinutes =
      static_cast<uint16_t>(localTime.tm_hour) * 60 + localTime.tm_min;
  const uint16_t startMinutes =
      static_cast<uint16_t>(automaticSettings.startHour) * 60 +
      automaticSettings.startMinute;
  if (currentMinutes < startMinutes) {
    setStatus("TIME WAIT");
    return AutomaticControlCommand::kNone;
  }

  if (automaticSettings.triggerMode == AutomaticTriggerMode::kTimeOnly) {
    if (nowMs - lastAutomaticAttemptMs < 30000 &&
        lastAutomaticAttemptMs != 0) {
      setStatus("ON READY");
      return AutomaticControlCommand::kNone;
    }
    lastAutomaticAttemptMs = nowMs;
    setStatus("ON READY");
    return AutomaticControlCommand::kSendOn;
  }

  if (isnan(temperatureC)) {
    setStatus("SENSOR ERR");
    return AutomaticControlCommand::kNone;
  }

  if (temperatureC > automaticSettings.onTemperatureC) {
    if (nowMs - lastAutomaticAttemptMs < 30000 &&
        lastAutomaticAttemptMs != 0) {
      setStatus("ON READY");
      return AutomaticControlCommand::kNone;
    }
    lastAutomaticAttemptMs = nowMs;
    setStatus("ON READY");
    return AutomaticControlCommand::kSendOn;
  }

  setStatus("TEMP WAIT");
  return AutomaticControlCommand::kNone;
}

void markAutomaticOnSent() {
  tm localTime = {};
  if (readKoreanTime(&localTime)) {
    lastSentDayKey = makeDayKey(localTime);
  }
  temperatureTriggerLatched = true;
  setStatus("ON SENT");
}

const char *getAutomaticControlStatus() { return statusText; }

AutomaticControlSettings getAutomaticControlSettings() {
  return automaticSettings;
}

bool saveAutomaticControlSettings(
    const AutomaticControlSettings &settings) {
  if (!settingsAreValid(settings)) {
    Serial.println("Automatic settings rejected: invalid value.");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("Automatic settings storage unavailable.");
    return false;
  }

  bool saved = preferences.putBool("enabled", settings.enabled) == 1;
  saved = (preferences.putUChar("hour", settings.startHour) == 1) && saved;
  saved = (preferences.putUChar("minute", settings.startMinute) == 1) && saved;
  saved = (preferences.putFloat("temp", settings.onTemperatureC) ==
           sizeof(float)) && saved;
  saved = (preferences.putUChar(
               "mode", static_cast<uint8_t>(settings.triggerMode)) == 1) &&
          saved;
  preferences.end();
  if (!saved) {
    Serial.println("Automatic settings save failed.");
    return false;
  }

  automaticSettings = settings;
  lastAutomaticAttemptMs = 0;
  Serial.printf("Automatic settings saved: %s, mode %u, start %02u:%02u, "
                "ON > %.1f C\n",
                automaticSettings.enabled ? "ON" : "OFF",
                static_cast<uint8_t>(automaticSettings.triggerMode),
                automaticSettings.startHour, automaticSettings.startMinute,
                automaticSettings.onTemperatureC);
  return true;
}

const char *getAutomaticOnProfileLabel() { return automaticOnProfileLabel; }

bool saveAutomaticOnProfileLabel(const char *label) {
  if (!profileLabelIsValid(label)) {
    Serial.println("Automatic profile rejected: invalid label.");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("Automatic profile storage unavailable.");
    return false;
  }
  const bool saved = preferences.putString("profile", label) == strlen(label);
  preferences.end();
  if (!saved) {
    Serial.println("Automatic profile save failed.");
    return false;
  }

  snprintf(automaticOnProfileLabel, sizeof(automaticOnProfileLabel), "%s",
           label);
  lastAutomaticAttemptMs = 0;
  Serial.printf("Automatic ON profile saved: %s\n", automaticOnProfileLabel);
  return true;
}

void printAutomaticControlConfiguration() {
  static const char *const kModeNames[] = {"BOTH", "TIME", "TEMP"};
  Serial.println("Automatic control configuration:");
  Serial.printf("  enabled : %s\n", automaticSettings.enabled ? "ON" : "OFF");
  Serial.printf("  mode    : %s\n",
                kModeNames[static_cast<uint8_t>(automaticSettings.triggerMode)]);
  Serial.printf("  start   : %02u:%02u\n", automaticSettings.startHour,
                automaticSettings.startMinute);
  Serial.printf("  temp    : %.1f C\n", automaticSettings.onTemperatureC);
  Serial.printf("  profile : %s\n", automaticOnProfileLabel);
}

bool getAutomaticControlClock(AutomaticControlClock *clock) {
  tm localTime = {};
  if (!clock || !readKoreanTime(&localTime)) {
    return false;
  }
  clock->year = static_cast<uint16_t>(localTime.tm_year + 1900);
  clock->month = static_cast<uint8_t>(localTime.tm_mon + 1);
  clock->day = static_cast<uint8_t>(localTime.tm_mday);
  clock->hour = static_cast<uint8_t>(localTime.tm_hour);
  clock->minute = static_cast<uint8_t>(localTime.tm_min);
  clock->second = static_cast<uint8_t>(localTime.tm_sec);
  return true;
}

AutomaticNetworkStatus getAutomaticNetworkStatus() {
  AutomaticNetworkStatus network = {};
  network.configured = wifiConfigured;
  network.connected = WiFi.status() == WL_CONNECTED;
  if (network.connected) {
    network.rssiDbm = static_cast<int16_t>(WiFi.RSSI());
    const String ipAddress = WiFi.localIP().toString();
    snprintf(network.ipAddress, sizeof(network.ipAddress), "%s",
             ipAddress.c_str());
  } else {
    snprintf(network.ipAddress, sizeof(network.ipAddress), "--");
  }
  return network;
}

void printWifiCredentialDiagnostics() {
  String fingerprintInput;
  fingerprintInput.reserve(strlen(kCredentialFingerprintPrefix) +
                           strlen(kWifiSsid) + strlen(kWifiPassword) + 2);
  fingerprintInput += kCredentialFingerprintPrefix;
  fingerprintInput += '\n';
  fingerprintInput += kWifiSsid;
  fingerprintInput += '\n';
  fingerprintInput += kWifiPassword;

  unsigned char digest[32] = {};
  const int hashResult = mbedtls_sha256_ret(
      reinterpret_cast<const unsigned char *>(fingerprintInput.c_str()),
      fingerprintInput.length(), digest, 0);

  Serial.println("Wi-Fi credential diagnostics:");
  Serial.printf("  secrets header: %s\n",
                kWifiSecretsHeaderPresent ? "present" : "missing");
  Serial.printf("  firmware build: %s %s\n", __DATE__, __TIME__);
  Serial.printf("  SSID: %s\n", kWifiSsid[0] ? kWifiSsid : "(empty)");
  Serial.printf("  password bytes: %u\n",
                static_cast<unsigned int>(strlen(kWifiPassword)));
  if (hashResult == 0) {
    Serial.printf("  fingerprint: %02X%02X%02X%02X\n", digest[0], digest[1],
                  digest[2], digest[3]);
  } else {
    Serial.printf("  fingerprint: ERROR (%d)\n", hashResult);
  }
  Serial.println("  Password text is never printed.");
}

void scanWifiNetworks() {
  Serial.println("Wi-Fi scan started...");
  WiFi.scanDelete();
  WiFi.disconnect(false, false);
  delay(100);
  const int16_t networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0) {
    Serial.printf("Wi-Fi scan failed: %d\n", networkCount);
    startWifi(millis());
    return;
  }
  if (networkCount == 0) {
    Serial.println("Wi-Fi scan complete: no networks found.");
    WiFi.scanDelete();
    startWifi(millis());
    return;
  }

  Serial.printf("Wi-Fi scan complete: %d network(s)\n", networkCount);
  for (int16_t index = 0; index < networkCount; ++index) {
    Serial.printf("  %2d. %-32s channel=%2d RSSI=%4d dBm auth=%d\n",
                  index + 1, WiFi.SSID(index).c_str(), WiFi.channel(index),
                  WiFi.RSSI(index), static_cast<int>(WiFi.encryptionType(index)));
  }
  WiFi.scanDelete();
  startWifi(millis());
}

void runWifiDetailedDiagnostics() {
  if (!wifiConfigured) {
    Serial.println("Wi-Fi detail unavailable: credentials are not configured.");
    return;
  }

  Serial.println();
  Serial.println("========== Wi-Fi detailed diagnostics ==========");
  Serial.printf("Firmware: Arduino ESP32 %d.%d.%d / ESP-IDF %s\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH, esp_get_idf_version());
  Serial.printf("Target SSID: %s (password bytes=%u; text hidden)\n", kWifiSsid,
                static_cast<unsigned int>(strlen(kWifiPassword)));
  printStationRadioConfiguration();

  // A single isolated attempt prevents the normal reconnect loop from mixing
  // unrelated disconnect events into this diagnostic result.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(250);

  Serial.println("Scanning only the configured SSID...");
  WiFi.scanDelete();
  const int16_t accessPointCount =
      WiFi.scanNetworks(false, true, false, 300, 0, kWifiSsid);
  Serial.printf("  scan result           : %d\n", accessPointCount);
  if (accessPointCount <= 0) {
    Serial.println("Diagnostic stopped: configured SSID was not detected.");
    WiFi.scanDelete();
    WiFi.setAutoReconnect(true);
    startWifi(millis());
    return;
  }

  const uint16_t recordsToRead = min(
      static_cast<uint16_t>(accessPointCount), kWifiDiagnosticMaxAccessPoints);
  wifi_ap_record_t selected = {};
  uint16_t selectedIndex = 0;
  for (uint16_t index = 0; index < recordsToRead; ++index) {
    const wifi_ap_record_t *record =
        static_cast<const wifi_ap_record_t *>(WiFi.getScanInfoByIndex(index));
    if (!record) {
      continue;
    }
    printAccessPointRecord(*record, index);
    if (index == 0 || record->rssi > selected.rssi) {
      selected = *record;
      selectedIndex = index;
    }
  }

  WiFi.scanDelete();
  if (!selected.ssid[0]) {
    Serial.println("Diagnostic stopped: target AP record was unavailable.");
    WiFi.setAutoReconnect(true);
    startWifi(millis());
    return;
  }

  Serial.printf("Selected strongest target AP #%u: ", selectedIndex + 1);
  printMacAddress(selected.bssid);
  Serial.printf(" channel=%u RSSI=%d dBm\n", selected.primary, selected.rssi);

  // Match the AP's advertised regulatory domain and compatibility settings
  // during this controlled test. Channel 11 is valid in both the original CN
  // and AP's US domain, but matching them removes that variable from diagnosis.
  wifi_country_t diagnosticCountry = {};
  memcpy(diagnosticCountry.cc, selected.country.cc,
         sizeof(diagnosticCountry.cc));
  diagnosticCountry.schan = selected.country.schan;
  diagnosticCountry.nchan = selected.country.nchan;
  diagnosticCountry.max_tx_power = selected.country.max_tx_power;
  diagnosticCountry.policy = WIFI_COUNTRY_POLICY_MANUAL;
  const esp_err_t countryResult = esp_wifi_set_country(&diagnosticCountry);
  const esp_err_t protocolResult = esp_wifi_set_protocol(
      WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  const esp_err_t bandwidthResult =
      esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  const bool sleepDisabled = WiFi.setSleep(false);
  printEspError("match AP country", countryResult);
  printEspError("force 11b/g/n", protocolResult);
  printEspError("force HT20", bandwidthResult);
  Serial.printf("  disable power save    : %s\n",
                sleepDisabled ? "OK" : "FAILED");
  printStationRadioConfiguration();

  wifiDiagnosticDisconnectCount = 0;
  wifiDiagnosticLastReason = 0;
  wifiDiagnosticLastRssi = 0;
  wifiDiagnosticStartedMs = millis();
  wifiDiagnosticActive = true;

  Serial.println("Starting one connection attempt pinned to BSSID/channel...");
  const wl_status_t beginStatus =
      WiFi.begin(kWifiSsid, kWifiPassword, selected.primary, selected.bssid,
                 true);
  Serial.printf("WiFi.begin status: %d\n", static_cast<int>(beginStatus));
  printStationConnectionConfiguration();

  while (millis() - wifiDiagnosticStartedMs < kWifiDiagnosticTimeoutMs &&
         WiFi.status() != WL_CONNECTED) {
    delay(50);
  }

  const uint32_t elapsedMs = millis() - wifiDiagnosticStartedMs;
  const bool connected = WiFi.status() == WL_CONNECTED;
  Serial.println("---------- Wi-Fi diagnostic summary ----------");
  Serial.printf("  result/status         : %s / %d\n",
                connected ? "CONNECTED" : "FAILED",
                static_cast<int>(WiFi.status()));
  Serial.printf("  elapsed/disconnects   : %lu ms / %u\n",
                static_cast<unsigned long>(elapsedMs),
                wifiDiagnosticDisconnectCount);
  if (wifiDiagnosticLastReason) {
    Serial.printf("  last reason           : %u (%s)\n",
                  wifiDiagnosticLastReason,
                  WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(
                      wifiDiagnosticLastReason)));
    Serial.printf("  last disconnect RSSI  : %d dBm\n",
                  wifiDiagnosticLastRssi);
  }

  if (connected) {
    wifi_ap_record_t connectedAccessPoint = {};
    const esp_err_t apInfoResult =
        esp_wifi_sta_get_ap_info(&connectedAccessPoint);
    printEspError("connected AP info", apInfoResult);
    if (apInfoResult == ESP_OK) {
      printAccessPointRecord(connectedAccessPoint, 0);
    }
    Serial.printf("  IP / gateway          : %s / %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());
    Serial.printf("  subnet / DNS          : %s / %s\n",
                  WiFi.subnetMask().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());
  }
  Serial.println("================================================");

  wifiDiagnosticActive = false;
  WiFi.setAutoReconnect(true);
  if (!connected) {
    WiFi.disconnect(false, false);
    startWifi(millis());
  }
}
