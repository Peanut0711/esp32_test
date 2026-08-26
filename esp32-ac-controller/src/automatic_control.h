#pragma once

#include <stdint.h>

enum class AutomaticControlCommand : uint8_t {
  kNone,
  kSendOn,
};

struct AutomaticControlSettings {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  float onTemperatureC;
};

struct AutomaticControlClock {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

void setupAutomaticControl();
AutomaticControlCommand pollAutomaticControl(float temperatureC);
void markAutomaticOnSent();
const char *getAutomaticControlStatus();
AutomaticControlSettings getAutomaticControlSettings();
bool saveAutomaticControlSettings(const AutomaticControlSettings &settings);
bool getAutomaticControlClock(AutomaticControlClock *clock);
