#include "automatic_control.h"

#include <Arduino.h>
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

constexpr float kAutomaticOnTemperatureC = 28.0F;
constexpr uint8_t kAutomaticStartHour = 6;
constexpr uint32_t kWifiRetryIntervalMs = 30000;
constexpr char kKoreanTimezone[] = "KST-9";

bool automaticControlEnabled = false;
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

}  // namespace

void setupAutomaticControl() {
  automaticControlEnabled = kWifiSsid[0] != '\0';
  if (!automaticControlEnabled) {
    Serial.println(
        "Automatic control disabled: create include/wifi_secrets.h first.");
    setStatus("NO WIFI CFG");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  startWifi(millis());
}

AutomaticControlCommand pollAutomaticControl(float temperatureC) {
  if (!automaticControlEnabled) {
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

  const int32_t todayKey = makeDayKey(localTime);
  if (lastSentDayKey == todayKey) {
    setStatus("ON SENT");
    return AutomaticControlCommand::kNone;
  }

  if (localTime.tm_hour < kAutomaticStartHour) {
    setStatus("WAIT 06:00");
    return AutomaticControlCommand::kNone;
  }

  if (isnan(temperatureC)) {
    setStatus("SENSOR ERR");
    return AutomaticControlCommand::kNone;
  }

  if (temperatureC > kAutomaticOnTemperatureC) {
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

bool getAutomaticControlClock(uint8_t *hour, uint8_t *minute) {
  tm localTime = {};
  if (!hour || !minute || !readKoreanTime(&localTime)) {
    return false;
  }
  *hour = static_cast<uint8_t>(localTime.tm_hour);
  *minute = static_cast<uint8_t>(localTime.tm_min);
  return true;
}
