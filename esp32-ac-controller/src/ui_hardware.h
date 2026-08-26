#pragma once

#include <stdint.h>

enum class UiCommand : uint8_t {
  kNone,
  kSendOn,
  kSendOff,
  kStartLearning,
  kCancelLearning,
};

void setupUiHardware();
UiCommand pollUiHardware();
void setUiLastAction(const char *action);
void setUiLearningProgress(const char *label, uint8_t captured,
                           uint8_t required, bool acceptingSignals);
void showUiLearningStartError(const char *label);
void clearUiLearningStatus();
const char *getUiLearningRequestLabel();
