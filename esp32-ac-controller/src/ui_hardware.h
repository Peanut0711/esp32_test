#pragma once

#include "automatic_control.h"

#include <stdint.h>

enum class UiCommand : uint8_t {
  kNone,
  kSendOn,
  kSendOff,
  kSendCustom,
  kSaveAutomaticProfile,
  kStartLearning,
  kStartCustomLearning,
  kEraseCustomLearning,
  kOverwriteCustomLearning,
  kCancelLearning,
  kSaveAutomaticSettings,
};

enum class UiTransmitMode : uint8_t {
  kCool,
  kFan,
  kHeat,
};

enum class UiTransmitFan : uint8_t {
  k1,
  k2,
  k3,
  kAuto,
};

struct UiTransmitSettings {
  bool power;
  UiTransmitMode mode;
  uint8_t temperatureC;
  UiTransmitFan fan;
  bool swing;
  bool turbo;
};

void setupUiHardware();
UiCommand pollUiHardware();
void setUiLastAction(const char *action);
void setUiLearningProgress(const char *label, uint8_t captured,
                           uint8_t required, bool acceptingSignals);
void showUiLearningStartError(const char *label);
void clearUiLearningStatus();
const char *getUiLearningRequestLabel();
const char *getUiTransmitRequestLabel();
UiTransmitSettings getUiTransmitSettings();
void setUiTransmitResult(bool sent, const char *label);
void setUiAutomaticProfileResult(bool saved, const char *label);
void showUiCustomLearningExists(const char *label);
void setUiCustomEraseResult(bool erased, const char *label);
float getUiTemperatureC();
void setUiAutomaticControlState(const char *status, bool clockValid,
                                const AutomaticControlClock &clock,
                                const AutomaticControlSettings &settings,
                                const AutomaticNetworkStatus &network);
AutomaticControlSettings getUiAutomaticSettings();
