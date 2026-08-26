#pragma once

#include <stdint.h>

enum class AutomaticControlCommand : uint8_t {
  kNone,
  kSendOn,
};

void setupAutomaticControl();
AutomaticControlCommand pollAutomaticControl(float temperatureC);
void markAutomaticOnSent();
const char *getAutomaticControlStatus();
bool getAutomaticControlClock(uint8_t *hour, uint8_t *minute);
