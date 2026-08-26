#pragma once

#include <stdint.h>

enum class UiCommand : uint8_t {
  kNone,
  kSendOn,
  kSendOff,
};

void setupUiHardware();
UiCommand pollUiHardware();
void setUiLastAction(const char *action);
