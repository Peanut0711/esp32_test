#pragma once

#include <stdint.h>

enum class AutomaticControlCommand : uint8_t {
  kNone,
  kSendOn,
};

enum class AutomaticTriggerMode : uint8_t {
  kTimeAndTemperature,
  kTimeOnly,
  kTemperatureOnly,
};

struct AutomaticControlSettings {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  float onTemperatureC;
  AutomaticTriggerMode triggerMode;
};

struct AutomaticControlClock {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

struct AutomaticNetworkStatus {
  bool configured;
  bool connected;
  int16_t rssiDbm;
  char ipAddress[16];
};

void setupAutomaticControl();
AutomaticControlCommand pollAutomaticControl(float temperatureC);
void markAutomaticOnSent();
const char *getAutomaticControlStatus();
AutomaticControlSettings getAutomaticControlSettings();
bool saveAutomaticControlSettings(const AutomaticControlSettings &settings);
const char *getAutomaticOnProfileLabel();
bool saveAutomaticOnProfileLabel(const char *label);
void printAutomaticControlConfiguration();
bool getAutomaticControlClock(AutomaticControlClock *clock);
AutomaticNetworkStatus getAutomaticNetworkStatus();
void printWifiCredentialDiagnostics();
void scanWifiNetworks();
void runWifiDetailedDiagnostics();
