#include "automatic_control.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
constexpr char kWifiSsid[] = "";
constexpr char kWifiPassword[] = "";
#endif

namespace {

constexpr uint32_t kWifiRetryIntervalMs = 30000;
constexpr char kKoreanTimezone[] = "KST-9";
constexpr char kPreferencesNamespace[] = "ac-auto";

AutomaticControlSettings automaticSettings = {true, 6, 0, 28.0F};
bool wifiConfigured = false;
bool ntpConfigured = false;
int32_t lastSentDayKey = -1;
uint32_t lastWifiAttemptMs = 0;
char statusText[16] = "DISABLED";

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
         settings.onTemperatureC <= 35.0F;
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
      preferences.getFloat("temp", automaticSettings.onTemperatureC)};
  preferences.end();
  if (settingsAreValid(loaded)) {
    automaticSettings = loaded;
  } else {
    Serial.println("Invalid automatic settings found; using defaults.");
  }
}

}  // namespace

void setupAutomaticControl() {
  loadSettings();
  Serial.printf("Automatic settings: %s, start %02u:%02u, ON > %.1f C\n",
                automaticSettings.enabled ? "ON" : "OFF",
                automaticSettings.startHour, automaticSettings.startMinute,
                automaticSettings.onTemperatureC);

  wifiConfigured = kWifiSsid[0] != '\0';
  if (!wifiConfigured) {
    Serial.println(
        "Automatic control disabled: create include/wifi_secrets.h first.");
    setStatus("NO WIFI CFG");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        Serial.printf("Wi-Fi disconnected: reason=%u\n",
                      info.wifi_sta_disconnected.reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  startWifi(millis());
}

AutomaticControlCommand pollAutomaticControl(float temperatureC) {
  if (!wifiConfigured) {
    return AutomaticControlCommand::kNone;
  }

  const uint32_t nowMs = millis();
  if (WiFi.status() != WL_CONNECTED) {
    ntpConfigured = false;
    setStatus("WIFI WAIT");
    if (nowMs - lastWifiAttemptMs >= kWifiRetryIntervalMs) {
      startWifi(nowMs);
    }
    return AutomaticControlCommand::kNone;
  }

  if (!ntpConfigured) {
    configTzTime(kKoreanTimezone, "pool.ntp.org", "time.google.com",
                 "time.cloudflare.com");
    ntpConfigured = true;
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("NTP synchronization requested (Asia/Seoul).");
  }

  tm localTime = {};
  if (!readKoreanTime(&localTime)) {
    setStatus("TIME SYNC");
    return AutomaticControlCommand::kNone;
  }

  if (!automaticSettings.enabled) {
    setStatus("AUTO OFF");
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

  if (isnan(temperatureC)) {
    setStatus("SENSOR ERR");
    return AutomaticControlCommand::kNone;
  }

  if (temperatureC > automaticSettings.onTemperatureC) {
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

  const bool saved = preferences.putBool("enabled", settings.enabled) == 1 &&
                     preferences.putUChar("hour", settings.startHour) == 1 &&
                     preferences.putUChar("minute", settings.startMinute) == 1 &&
                     preferences.putFloat("temp", settings.onTemperatureC) ==
                         sizeof(float);
  preferences.end();
  if (!saved) {
    Serial.println("Automatic settings save failed.");
    return false;
  }

  automaticSettings = settings;
  Serial.printf("Automatic settings saved: %s, start %02u:%02u, ON > %.1f C\n",
                automaticSettings.enabled ? "ON" : "OFF",
                automaticSettings.startHour, automaticSettings.startMinute,
                automaticSettings.onTemperatureC);
  return true;
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
