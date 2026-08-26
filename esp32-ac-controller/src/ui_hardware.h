#pragma once

#include "automatic_control.h"

#include <stdint.h>

enum class UiCommand : uint8_t {
  kNone,
  kSendOn,
  kSendOff,
  kStartLearning,
  kCancelLearning,
  kSaveAutomaticSettings,
};

void setupUiHardware();
UiCommand pollUiHardware();
void setUiLastAction(const char *action);
void setUiLearningProgress(const char *label, uint8_t captured,
                           uint8_t required, bool acceptingSignals);
void showUiLearningStartError(const char *label);
void clearUiLearningStatus();
const char *getUiLearningRequestLabel();
float getUiTemperatureC();
void setUiAutomaticControlState(const char *status, bool clockValid,
                                const AutomaticControlClock &clock,
                                const AutomaticControlSettings &settings);
AutomaticControlSettings getUiAutomaticSettings();
