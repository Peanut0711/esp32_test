#include "ui_hardware.h"

#include <Adafruit_SH110X.h>
#include <Adafruit_SHT4x.h>
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <string.h>

namespace {

constexpr uint8_t kSdaPin = 21;
constexpr uint8_t kSclPin = 20;
constexpr uint8_t kHeartbeatLedPin = 8;
constexpr uint8_t kEncoderAPin = 5;
constexpr uint8_t kEncoderBPin = 6;
constexpr uint8_t kEncoderPushPin = 7;
constexpr uint8_t kConfirmPin = 10;
constexpr uint8_t kBackPin = 1;

constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kSht40Address = 0x44;
constexpr uint32_t kButtonDebounceMs = 25;
constexpr uint32_t kSensorIntervalMs = 1000;
constexpr uint32_t kSensorLogIntervalMs = 10000;
constexpr uint32_t kDisplayIntervalMs = 100;
constexpr uint32_t kHeartbeatToggleIntervalMs = 500;
constexpr uint8_t kEncoderDiagnosticQueueSize = 32;
constexpr uint8_t kEncoderDetentState = 0b11;
constexpr int8_t kEncoderMinimumDetentEdges = 2;

enum class UiScreen : uint8_t {
  kMain,
  kMainMenu,
  kTransmitMenu,
  kLearnCategory,
  kLearnTarget,
  kClock,
  kAutomaticSettings,
};

enum class MainMenuSelection : uint8_t {
  kTransmit,
  kLearn,
  kClock,
  kAutomaticSettings,
  kCount,
};

enum class AutomaticSettingSelection : uint8_t {
  kEnabled,
  kTriggerMode,
  kStartTime,
  kOnTemperature,
  kPowerSave,
  kWakeInterval,
  kSave,
  kCount,
};

enum class TransmitSettingSelection : uint8_t {
  kPower,
  kMode,
  kTemperature,
  kFan,
  kSwing,
  kTurbo,
  kSend,
  kAutoProfile,
  kLearn,
  kCount,
};

enum class LearnCategory : uint8_t {
  kPower,
  kMode,
  kTemperature,
  kFan,
  kSwing,
  kTurbo,
  kTimer,
  kCount,
};

struct DebouncedButton {
  uint8_t pin;
  bool rawState;
  bool stableState;
  uint32_t lastRawChangeMs;
};

struct EncoderDiagnosticEvent {
  uint8_t priorState;
  uint8_t currentState;
  int8_t edge;
  int8_t accumulatedSteps;
  int8_t move;
};

Adafruit_SH1106G oled(128, 64, &Wire, -1);
Adafruit_SHT4x sht40;

DebouncedButton encoderPush = {kEncoderPushPin, HIGH, HIGH, 0};
DebouncedButton confirmButton = {kConfirmPin, HIGH, HIGH, 0};
DebouncedButton backButton = {kBackPin, HIGH, HIGH, 0};

bool oledReady = false;
bool sht40Ready = false;
float temperatureC = NAN;
float humidityPercent = NAN;
char lastAction[16] = "BOOT";
char automaticControlStatus[16] = "NO WIFI CFG";
bool automaticClockValid = false;
AutomaticControlClock automaticClock = {};
AutomaticNetworkStatus automaticNetwork = {};
AutomaticControlSettings appliedAutomaticSettings = {
    true, 6, 0, 28.0F, AutomaticTriggerMode::kTimeAndTemperature, false, 3};
AutomaticControlSettings draftAutomaticSettings = {
    true, 6, 0, 28.0F, AutomaticTriggerMode::kTimeAndTemperature, false, 3};
bool uiInteractiveEnabled = true;
volatile bool encoderDiagnosticsEnabled = false;
bool heartbeatLedOn = false;
uint32_t lastHeartbeatToggleMs = 0;
uint32_t lastUiInteractionMs = 0;
UiScreen currentScreen = UiScreen::kMain;
UiScreen automaticSettingsReturnScreen = UiScreen::kMain;
MainMenuSelection mainMenuSelection = MainMenuSelection::kTransmit;
AutomaticSettingSelection automaticSettingSelection =
    AutomaticSettingSelection::kEnabled;
bool automaticSettingEditing = false;
uint8_t automaticTimeEditField = 0;
TransmitSettingSelection transmitSettingSelection =
    TransmitSettingSelection::kPower;
bool transmitSettingEditing = false;
UiTransmitSettings transmitSettings = {
    true, UiTransmitMode::kCool, 27, UiTransmitFan::k1, true, false};
char transmitRequestLabel[32] = "";
char transmitFeedback[24] = "";
bool customRecordActionVisible = false;
bool customRecordResultVisible = false;
bool customRecordEraseSucceeded = false;
char customRecordLabel[32] = "";
LearnCategory learnCategory = LearnCategory::kPower;
int8_t learnTargetValues[static_cast<uint8_t>(LearnCategory::kCount)] = {
    1, 0, 26, 0, 1, 0, 0};
char learningRequestLabel[32] = "";
bool learningStatusVisible = false;
bool learningAcceptingSignals = false;
bool learningStartError = false;
char learningLabel[32] = "";
uint8_t learningCaptured = 0;
uint8_t learningRequired = 0;

portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR int8_t encoderTransitionTable[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile uint8_t encoderPreviousState = 0;
volatile int8_t encoderQuarterSteps = 0;
volatile int16_t encoderPendingMoves = 0;
volatile uint16_t encoderActivityCount = 0;
volatile EncoderDiagnosticEvent
    encoderDiagnosticQueue[kEncoderDiagnosticQueueSize];
volatile uint8_t encoderDiagnosticHead = 0;
volatile uint8_t encoderDiagnosticTail = 0;
volatile uint16_t encoderDiagnosticDropped = 0;
uint32_t lastSensorReadMs = 0;
uint32_t lastSensorLogMs = 0;
uint32_t lastDisplayDrawMs = 0;

bool probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void initializeButton(DebouncedButton &button) {
  pinMode(button.pin, INPUT_PULLUP);
  button.rawState = digitalRead(button.pin);
  button.stableState = button.rawState;
  button.lastRawChangeMs = millis();
}

bool updateButton(DebouncedButton &button, uint32_t nowMs) {
  const bool rawState = digitalRead(button.pin);
  if (rawState != button.rawState) {
    button.rawState = rawState;
    button.lastRawChangeMs = nowMs;
  }

  if (rawState != button.stableState &&
      nowMs - button.lastRawChangeMs >= kButtonDebounceMs) {
    button.stableState = rawState;
    return button.stableState == LOW;
  }

  return false;
}

void IRAM_ATTR handleEncoderEdge() {
  const uint8_t currentState =
      (static_cast<uint8_t>(gpio_get_level(
           static_cast<gpio_num_t>(kEncoderAPin)))
       << 1) |
      static_cast<uint8_t>(
          gpio_get_level(static_cast<gpio_num_t>(kEncoderBPin)));

  portENTER_CRITICAL_ISR(&encoderMux);
  const uint8_t priorState = encoderPreviousState;
  if (currentState == priorState) {
    portEXIT_CRITICAL_ISR(&encoderMux);
    return;
  }

  const int8_t edge =
      encoderTransitionTable[(priorState << 2) | currentState];
  encoderQuarterSteps += edge;
  encoderPreviousState = currentState;
  ++encoderActivityCount;

  int8_t move = 0;
  const int8_t accumulatedSteps = encoderQuarterSteps;
  if (currentState == kEncoderDetentState) {
    if (encoderQuarterSteps >= kEncoderMinimumDetentEdges) {
      move = 1;
      ++encoderPendingMoves;
    } else if (encoderQuarterSteps <= -kEncoderMinimumDetentEdges) {
      move = -1;
      --encoderPendingMoves;
    }
    // Returning to the mechanical detent completes or cancels this click.
    // Clearing here prevents an incomplete click from shifting every later one.
    encoderQuarterSteps = 0;
  }

  if (encoderDiagnosticsEnabled) {
    const uint8_t nextHead = static_cast<uint8_t>(
        (encoderDiagnosticHead + 1) % kEncoderDiagnosticQueueSize);
    if (nextHead != encoderDiagnosticTail) {
      volatile EncoderDiagnosticEvent &event =
          encoderDiagnosticQueue[encoderDiagnosticHead];
      event.priorState = priorState;
      event.currentState = currentState;
      event.edge = edge;
      event.accumulatedSteps = accumulatedSteps;
      event.move = move;
      encoderDiagnosticHead = nextHead;
    } else {
      ++encoderDiagnosticDropped;
    }
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void setHeartbeatLed(bool enabled) {
  heartbeatLedOn = enabled;
  // The ESP32-C3 SuperMini onboard blue LED is active-low on GPIO8.
  digitalWrite(kHeartbeatLedPin, enabled ? LOW : HIGH);
}

void updateHeartbeat(uint32_t nowMs) {
  if (!uiInteractiveEnabled ||
      nowMs - lastHeartbeatToggleMs < kHeartbeatToggleIntervalMs) {
    return;
  }
  lastHeartbeatToggleMs = nowMs;
  setHeartbeatLed(!heartbeatLedOn);
}

void printPendingEncoderDiagnostics() {
  while (true) {
    EncoderDiagnosticEvent event = {};
    uint16_t dropped = 0;
    bool available = false;
    portENTER_CRITICAL(&encoderMux);
    if (encoderDiagnosticTail != encoderDiagnosticHead) {
      const volatile EncoderDiagnosticEvent &queued =
          encoderDiagnosticQueue[encoderDiagnosticTail];
      event = {queued.priorState, queued.currentState, queued.edge,
               queued.accumulatedSteps, queued.move};
      encoderDiagnosticTail = static_cast<uint8_t>(
          (encoderDiagnosticTail + 1) % kEncoderDiagnosticQueueSize);
      available = true;
    } else if (encoderDiagnosticDropped != 0) {
      dropped = encoderDiagnosticDropped;
      encoderDiagnosticDropped = 0;
    }
    portEXIT_CRITICAL(&encoderMux);

    if (available) {
      Serial.printf(
          "ENC prev=%u%u curr=%u%u edge=%+d sum=%+d move=%+d%s\n",
          (event.priorState >> 1) & 1, event.priorState & 1,
          (event.currentState >> 1) & 1, event.currentState & 1,
          event.edge, event.accumulatedSteps, event.move,
          event.edge == 0 ? " INVALID" : "");
      continue;
    }
    if (dropped != 0) {
      Serial.printf("ENC diagnostic queue dropped %u event(s).\n", dropped);
    }
    break;
  }
}

int8_t readEncoderChange() {
  int8_t change = 0;
  bool encoderWasActive = false;
  portENTER_CRITICAL(&encoderMux);
  if (encoderPendingMoves > 0) {
    change = 1;
    --encoderPendingMoves;
  } else if (encoderPendingMoves < 0) {
    change = -1;
    ++encoderPendingMoves;
  }
  encoderWasActive = encoderActivityCount != 0;
  encoderActivityCount = 0;
  portEXIT_CRITICAL(&encoderMux);

  if (encoderWasActive) {
    lastUiInteractionMs = millis();
  }
  printPendingEncoderDiagnostics();

  return change;
}

const char *getLearnCategoryName(LearnCategory category) {
  switch (category) {
    case LearnCategory::kPower:
      return "POWER";
    case LearnCategory::kMode:
      return "MODE";
    case LearnCategory::kTemperature:
      return "TEMP";
    case LearnCategory::kFan:
      return "FAN";
    case LearnCategory::kSwing:
      return "SWING";
    case LearnCategory::kTurbo:
      return "TURBO";
    case LearnCategory::kTimer:
      return "TIME OFF";
    case LearnCategory::kCount:
      break;
  }
  return "?";
}

int8_t wrapValue(int8_t value, int8_t minimum, int8_t maximum) {
  if (value > maximum) {
    return minimum;
  }
  if (value < minimum) {
    return maximum;
  }
  return value;
}

const char *getMainMenuName(MainMenuSelection selection) {
  switch (selection) {
    case MainMenuSelection::kTransmit:
      return "IR TRANSMIT";
    case MainMenuSelection::kLearn:
      return "IR LEARN";
    case MainMenuSelection::kClock:
      return "CLOCK";
    case MainMenuSelection::kAutomaticSettings:
      return "AUTO SETTINGS";
    case MainMenuSelection::kCount:
      break;
  }
  return "?";
}

void openAutomaticSettings(UiScreen returnScreen) {
  automaticSettingsReturnScreen = returnScreen;
  currentScreen = UiScreen::kAutomaticSettings;
  automaticSettingSelection = AutomaticSettingSelection::kEnabled;
  automaticSettingEditing = false;
  automaticTimeEditField = 0;
  draftAutomaticSettings = appliedAutomaticSettings;
}

const char *getTransmitModeName(UiTransmitMode mode) {
  static const char *const kNames[] = {"COOL", "FAN", "HEAT"};
  return kNames[static_cast<uint8_t>(mode)];
}

const char *getTransmitFanName(UiTransmitFan fan) {
  static const char *const kNames[] = {"F1", "F2", "F3", "AUTO"};
  return kNames[static_cast<uint8_t>(fan)];
}

const char *getAutomaticTriggerName(AutomaticTriggerMode mode) {
  static const char *const kNames[] = {"BOTH", "TIME", "TEMP"};
  return kNames[static_cast<uint8_t>(mode)];
}

void buildTransmitRequestLabel() {
  if (!transmitSettings.power) {
    snprintf(transmitRequestLabel, sizeof(transmitRequestLabel), "power_off");
    return;
  }

  static const char *const kModeLabels[] = {"c", "f", "h"};
  static const char *const kFanLabels[] = {"1", "2", "3", "a"};
  snprintf(transmitRequestLabel, sizeof(transmitRequestLabel),
           "ac_%s_%02u_%s_s%u_t%u",
           kModeLabels[static_cast<uint8_t>(transmitSettings.mode)],
           transmitSettings.temperatureC,
           kFanLabels[static_cast<uint8_t>(transmitSettings.fan)],
           transmitSettings.swing, transmitSettings.turbo);
}

void adjustTransmitSetting(int8_t change) {
  transmitFeedback[0] = '\0';
  switch (transmitSettingSelection) {
    case TransmitSettingSelection::kPower:
      transmitSettings.power = !transmitSettings.power;
      break;
    case TransmitSettingSelection::kMode:
      transmitSettings.mode = static_cast<UiTransmitMode>(wrapValue(
          static_cast<int8_t>(transmitSettings.mode) + change, 0, 2));
      break;
    case TransmitSettingSelection::kTemperature:
      transmitSettings.temperatureC = static_cast<uint8_t>(wrapValue(
          static_cast<int8_t>(transmitSettings.temperatureC) + change, 16,
          30));
      break;
    case TransmitSettingSelection::kFan:
      transmitSettings.fan = static_cast<UiTransmitFan>(wrapValue(
          static_cast<int8_t>(transmitSettings.fan) + change, 0, 3));
      break;
    case TransmitSettingSelection::kSwing:
      transmitSettings.swing = !transmitSettings.swing;
      break;
    case TransmitSettingSelection::kTurbo:
      transmitSettings.turbo = !transmitSettings.turbo;
      break;
    case TransmitSettingSelection::kSend:
    case TransmitSettingSelection::kAutoProfile:
    case TransmitSettingSelection::kLearn:
    case TransmitSettingSelection::kCount:
      break;
  }
}

UiCommand activateTransmitSetting() {
  transmitFeedback[0] = '\0';
  switch (transmitSettingSelection) {
    case TransmitSettingSelection::kPower:
    case TransmitSettingSelection::kMode:
    case TransmitSettingSelection::kTemperature:
    case TransmitSettingSelection::kFan:
    case TransmitSettingSelection::kSwing:
    case TransmitSettingSelection::kTurbo:
      transmitSettingEditing = !transmitSettingEditing;
      return UiCommand::kNone;
    case TransmitSettingSelection::kSend:
      buildTransmitRequestLabel();
      return UiCommand::kSendCustom;
    case TransmitSettingSelection::kAutoProfile:
      buildTransmitRequestLabel();
      return UiCommand::kSaveAutomaticProfile;
    case TransmitSettingSelection::kLearn:
      buildTransmitRequestLabel();
      return UiCommand::kStartCustomLearning;
    case TransmitSettingSelection::kCount:
      break;
  }
  return UiCommand::kNone;
}

void enterSelectedMainMenu() {
  switch (mainMenuSelection) {
    case MainMenuSelection::kTransmit:
      currentScreen = UiScreen::kTransmitMenu;
      transmitSettingSelection = TransmitSettingSelection::kPower;
      transmitSettingEditing = false;
      transmitFeedback[0] = '\0';
      break;
    case MainMenuSelection::kLearn:
      currentScreen = UiScreen::kLearnCategory;
      break;
    case MainMenuSelection::kClock:
      currentScreen = UiScreen::kClock;
      break;
    case MainMenuSelection::kAutomaticSettings:
      openAutomaticSettings(UiScreen::kMainMenu);
      break;
    case MainMenuSelection::kCount:
      break;
  }
}

void adjustAutomaticSetting(int8_t change) {
  switch (automaticSettingSelection) {
    case AutomaticSettingSelection::kEnabled:
      draftAutomaticSettings.enabled = !draftAutomaticSettings.enabled;
      break;
    case AutomaticSettingSelection::kTriggerMode:
      draftAutomaticSettings.triggerMode =
          static_cast<AutomaticTriggerMode>(wrapValue(
              static_cast<int8_t>(draftAutomaticSettings.triggerMode) +
                  change,
              0, 2));
      break;
    case AutomaticSettingSelection::kStartTime:
      if (automaticTimeEditField == 0) {
        draftAutomaticSettings.startHour = static_cast<uint8_t>(wrapValue(
            static_cast<int8_t>(draftAutomaticSettings.startHour) + change, 0,
            23));
      } else {
        draftAutomaticSettings.startMinute = static_cast<uint8_t>(wrapValue(
            static_cast<int8_t>(draftAutomaticSettings.startMinute) + change,
            0, 59));
      }
      break;
    case AutomaticSettingSelection::kOnTemperature:
      draftAutomaticSettings.onTemperatureC = constrain(
          draftAutomaticSettings.onTemperatureC + change * 0.5F, 16.0F,
          35.0F);
      break;
    case AutomaticSettingSelection::kPowerSave:
      draftAutomaticSettings.powerSaveEnabled =
          !draftAutomaticSettings.powerSaveEnabled;
      break;
    case AutomaticSettingSelection::kWakeInterval:
      draftAutomaticSettings.wakeIntervalMinutes =
          static_cast<uint8_t>(wrapValue(
              static_cast<int8_t>(
                  draftAutomaticSettings.wakeIntervalMinutes) +
                  change,
              1, 5));
      break;
    case AutomaticSettingSelection::kSave:
    case AutomaticSettingSelection::kCount:
      break;
  }
}

UiCommand activateAutomaticSetting() {
  switch (automaticSettingSelection) {
    case AutomaticSettingSelection::kEnabled:
      draftAutomaticSettings.enabled = !draftAutomaticSettings.enabled;
      break;
    case AutomaticSettingSelection::kTriggerMode:
      automaticSettingEditing = !automaticSettingEditing;
      break;
    case AutomaticSettingSelection::kStartTime:
      if (!automaticSettingEditing) {
        automaticSettingEditing = true;
        automaticTimeEditField = 0;
      } else if (automaticTimeEditField == 0) {
        automaticTimeEditField = 1;
      } else {
        automaticSettingEditing = false;
      }
      break;
    case AutomaticSettingSelection::kOnTemperature:
      automaticSettingEditing = !automaticSettingEditing;
      break;
    case AutomaticSettingSelection::kPowerSave:
      draftAutomaticSettings.powerSaveEnabled =
          !draftAutomaticSettings.powerSaveEnabled;
      break;
    case AutomaticSettingSelection::kWakeInterval:
      automaticSettingEditing = !automaticSettingEditing;
      break;
    case AutomaticSettingSelection::kSave:
      currentScreen = automaticSettingsReturnScreen;
      automaticSettingEditing = false;
      return UiCommand::kSaveAutomaticSettings;
    case AutomaticSettingSelection::kCount:
      break;
  }
  return UiCommand::kNone;
}

void adjustLearnTarget(int8_t change) {
  const uint8_t index = static_cast<uint8_t>(learnCategory);
  int8_t minimum = 0;
  int8_t maximum = 1;
  switch (learnCategory) {
    case LearnCategory::kMode:
      maximum = 2;
      break;
    case LearnCategory::kTemperature:
      minimum = 16;
      maximum = 30;
      break;
    case LearnCategory::kFan:
      maximum = 3;
      break;
    case LearnCategory::kTimer:
      maximum = 9;
      break;
    default:
      break;
  }
  learnTargetValues[index] =
      wrapValue(learnTargetValues[index] + change, minimum, maximum);
}

const char *formatLearnTarget(char *buffer, size_t bufferSize) {
  const int8_t value = learnTargetValues[static_cast<uint8_t>(learnCategory)];
  static const char *const kModeNames[] = {"COOL", "FAN", "HEAT"};
  static const char *const kFanNames[] = {"F1", "F2", "F3", "AUTO"};

  switch (learnCategory) {
    case LearnCategory::kPower:
      snprintf(buffer, bufferSize, "%s", value ? "ON" : "OFF");
      break;
    case LearnCategory::kMode:
      snprintf(buffer, bufferSize, "%s", kModeNames[value]);
      break;
    case LearnCategory::kTemperature:
      snprintf(buffer, bufferSize, "%dC", value);
      break;
    case LearnCategory::kFan:
      snprintf(buffer, bufferSize, "%s", kFanNames[value]);
      break;
    case LearnCategory::kSwing:
    case LearnCategory::kTurbo:
      snprintf(buffer, bufferSize, "%s", value ? "ON" : "OFF");
      break;
    case LearnCategory::kTimer:
      if (value) {
        snprintf(buffer, bufferSize, "%dH", value);
      } else {
        snprintf(buffer, bufferSize, "OFF");
      }
      break;
    case LearnCategory::kCount:
      snprintf(buffer, bufferSize, "?");
      break;
  }
  return buffer;
}

const char *getLearnBaseContext() {
  switch (learnCategory) {
    case LearnCategory::kPower:
      return "BASE:C27 F1 SW1";
    case LearnCategory::kMode:
      return "BASE:27 F1 SW1";
    case LearnCategory::kTemperature:
      return "BASE:C F1 SW1 TB0";
    case LearnCategory::kFan:
      return "BASE:C27 SW1 TB0";
    case LearnCategory::kSwing:
      return "BASE:C27 F1 TB0";
    case LearnCategory::kTurbo:
    case LearnCategory::kTimer:
      return "BASE:C27 F1 SW1";
    case LearnCategory::kCount:
      break;
  }
  return "";
}

void buildLearningRequestLabel() {
  const int8_t value = learnTargetValues[static_cast<uint8_t>(learnCategory)];
  static const char *const kModeLabels[] = {"cool", "fan", "heat"};
  static const char *const kFanLabels[] = {"f1", "f2", "f3", "fauto"};

  switch (learnCategory) {
    case LearnCategory::kPower:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel), "power_%s",
               value ? "on" : "off");
      break;
    case LearnCategory::kMode:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel),
               "mode_%s_27_f1_swing_on", kModeLabels[value]);
      break;
    case LearnCategory::kTemperature:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel),
               "cool_%d_f1_swing_on", value);
      break;
    case LearnCategory::kFan:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel),
               "cool_27_%s_swing_on", kFanLabels[value]);
      break;
    case LearnCategory::kSwing:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel),
               "cool_27_f1_swing_%s", value ? "on" : "off");
      break;
    case LearnCategory::kTurbo:
      snprintf(learningRequestLabel, sizeof(learningRequestLabel),
               "cool_27_f1_swing_on_turbo_%s", value ? "on" : "off");
      break;
    case LearnCategory::kTimer:
      if (value) {
        snprintf(learningRequestLabel, sizeof(learningRequestLabel),
                 "cool_27_f1_swing_on_timer_%dh", value);
      } else {
        snprintf(learningRequestLabel, sizeof(learningRequestLabel),
                 "cool_27_f1_swing_on_timer_off");
      }
      break;
    case LearnCategory::kCount:
      learningRequestLabel[0] = '\0';
      break;
  }
}

void readSensor(uint32_t nowMs) {
  if (!sht40Ready || nowMs - lastSensorReadMs < kSensorIntervalMs) {
    return;
  }

  lastSensorReadMs = nowMs;
  sensors_event_t humidityEvent;
  sensors_event_t temperatureEvent;
  if (sht40.getEvent(&humidityEvent, &temperatureEvent)) {
    temperatureC = temperatureEvent.temperature;
    humidityPercent = humidityEvent.relative_humidity;
    if (nowMs - lastSensorLogMs >= kSensorLogIntervalMs) {
      lastSensorLogMs = nowMs;
      Serial.printf("SHT40: %.2f C, %.2f %%RH\n", temperatureC,
                    humidityPercent);
    }
  } else {
    temperatureC = NAN;
    humidityPercent = NAN;
    Serial.println("SHT40 read failed.");
  }
}

void drawDisplay(uint32_t nowMs) {
  if (!oledReady || nowMs - lastDisplayDrawMs < kDisplayIntervalMs) {
    return;
  }

  lastDisplayDrawMs = nowMs;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setTextWrap(false);

  if (customRecordActionVisible) {
    oled.setCursor(0, 0);
    oled.println("SAVED IR EXISTS");
    oled.setCursor(0, 13);
    oled.printf("%.21s", customRecordLabel);
    oled.setCursor(0, 26);
    oled.println("PUSH: ERASE");
    oled.setCursor(0, 39);
    oled.println("CONFIRM: RELEARN");
    oled.setCursor(0, 52);
    oled.println("BACK: CANCEL");
    oled.display();
    return;
  }

  if (customRecordResultVisible) {
    oled.setCursor(0, 0);
    oled.println("IR RECORD");
    oled.setCursor(0, 13);
    oled.printf("%.21s", customRecordLabel);
    oled.setCursor(0, 30);
    oled.println(customRecordEraseSucceeded ? "ERASED" : "ERASE FAILED");
    oled.setCursor(0, 52);
    oled.println("ANY BUTTON: BACK");
    oled.display();
    return;
  }

  if (learningStatusVisible) {
    oled.setCursor(0, 0);
    oled.println(learningStartError ? "IR LEARN FAILED" : "IR LEARN");
    oled.setCursor(0, 13);
    oled.println(learningLabel);
    if (learningStartError) {
      oled.setCursor(0, 28);
      oled.println("ALREADY SAVED?");
      oled.setCursor(0, 43);
      oled.println("CHECK SERIAL / ERASE");
      oled.setCursor(0, 56);
      oled.println("ANY BUTTON: BACK");
      oled.display();
      return;
    }
    oled.setCursor(0, 28);
    oled.printf("SAMPLE %u / %u", learningCaptured, learningRequired);
    oled.setCursor(0, 43);
    oled.println(learningAcceptingSignals ? "PRESS REMOTE" : "LEARNING SAVED");
    oled.setCursor(0, 56);
    oled.println(learningAcceptingSignals ? "BACK: CANCEL"
                                          : "P:NEXT C:STAY B:TYPE");
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kMainMenu) {
    const uint8_t selected = static_cast<uint8_t>(mainMenuSelection);
    oled.setCursor(0, 0);
    oled.println("MAIN MENU");
    for (uint8_t row = 0;
         row < static_cast<uint8_t>(MainMenuSelection::kCount); ++row) {
      oled.setCursor(0, 14 + row * 12);
      oled.printf("%c %s", row == selected ? '>' : ' ',
                  getMainMenuName(static_cast<MainMenuSelection>(row)));
    }
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kClock) {
    oled.setCursor(0, 0);
    oled.println("CLOCK & WIFI");
    if (automaticClockValid) {
      oled.setCursor(0, 13);
      oled.printf("%04u-%02u-%02u", automaticClock.year,
                  automaticClock.month, automaticClock.day);
      oled.setCursor(0, 26);
      oled.printf("%02u:%02u:%02u KST", automaticClock.hour,
                  automaticClock.minute, automaticClock.second);
    } else {
      oled.setCursor(0, 19);
      oled.println("TIME NOT SYNCED");
    }
    oled.setCursor(0, 39);
    if (automaticNetwork.connected) {
      oled.printf("WIFI:OK %ddBm", automaticNetwork.rssiDbm);
    } else {
      oled.printf("WIFI:%s", automaticNetwork.configured ? "WAIT" : "NO CONFIG");
    }
    oled.setCursor(0, 52);
    oled.printf("IP:%s", automaticNetwork.ipAddress);
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kAutomaticSettings) {
    const uint8_t selected = static_cast<uint8_t>(automaticSettingSelection);
    const uint8_t itemCount =
        static_cast<uint8_t>(AutomaticSettingSelection::kCount);
    uint8_t first = selected > 1 ? selected - 1 : 0;
    if (first + 4 > itemCount) {
      first = itemCount - 4;
    }

    oled.setCursor(0, 0);
    oled.println("AUTO SETTINGS");
    for (uint8_t row = 0; row < 4; ++row) {
      const uint8_t index = first + row;
      const char marker = index == selected
                              ? (automaticSettingEditing ? '*' : '>')
                              : ' ';
      oled.setCursor(0, 14 + row * 12);
      switch (static_cast<AutomaticSettingSelection>(index)) {
        case AutomaticSettingSelection::kEnabled:
          oled.printf("%c ENABLE %s", marker,
                      draftAutomaticSettings.enabled ? "ON" : "OFF");
          break;
        case AutomaticSettingSelection::kTriggerMode:
          oled.printf("%c COND   %s", marker,
                      getAutomaticTriggerName(
                          draftAutomaticSettings.triggerMode));
          break;
        case AutomaticSettingSelection::kStartTime:
          oled.printf("%c START  %02u:%02u", marker,
                      draftAutomaticSettings.startHour,
                      draftAutomaticSettings.startMinute);
          if (index == selected && automaticSettingEditing) {
            oled.printf(" %c", automaticTimeEditField == 0 ? 'H' : 'M');
          }
          break;
        case AutomaticSettingSelection::kOnTemperature:
          oled.printf("%c TEMP   %.1fC", marker,
                      draftAutomaticSettings.onTemperatureC);
          break;
        case AutomaticSettingSelection::kPowerSave:
          oled.printf("%c SLEEP  %s", marker,
                      draftAutomaticSettings.powerSaveEnabled ? "ON" : "OFF");
          break;
        case AutomaticSettingSelection::kWakeInterval:
          oled.printf("%c WAKE   %u MIN", marker,
                      draftAutomaticSettings.wakeIntervalMinutes);
          break;
        case AutomaticSettingSelection::kSave:
          oled.printf("%c SAVE & EXIT", marker);
          break;
        case AutomaticSettingSelection::kCount:
          break;
      }
    }
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kTransmitMenu) {
    const uint8_t selected = static_cast<uint8_t>(transmitSettingSelection);
    const uint8_t itemCount =
        static_cast<uint8_t>(TransmitSettingSelection::kCount);
    uint8_t first = selected > 1 ? selected - 1 : 0;
    if (first + 4 > itemCount) {
      first = itemCount - 4;
    }

    oled.setCursor(0, 0);
    oled.println(transmitFeedback[0] ? transmitFeedback : "CUSTOM IR TX");
    for (uint8_t row = 0; row < 4; ++row) {
      const uint8_t index = first + row;
      const char marker = index == selected
                              ? (transmitSettingEditing ? '*' : '>')
                              : ' ';
      oled.setCursor(0, 14 + row * 12);
      switch (static_cast<TransmitSettingSelection>(index)) {
        case TransmitSettingSelection::kPower:
          oled.printf("%c POWER  %s", marker,
                      transmitSettings.power ? "ON" : "OFF");
          break;
        case TransmitSettingSelection::kMode:
          oled.printf("%c MODE   %s", marker,
                      getTransmitModeName(transmitSettings.mode));
          break;
        case TransmitSettingSelection::kTemperature:
          oled.printf("%c TEMP   %uC", marker,
                      transmitSettings.temperatureC);
          break;
        case TransmitSettingSelection::kFan:
          oled.printf("%c FAN    %s", marker,
                      getTransmitFanName(transmitSettings.fan));
          break;
        case TransmitSettingSelection::kSwing:
          oled.printf("%c SWING  %s", marker,
                      transmitSettings.swing ? "ON" : "OFF");
          break;
        case TransmitSettingSelection::kTurbo:
          oled.printf("%c TURBO  %s", marker,
                      transmitSettings.turbo ? "ON" : "OFF");
          break;
        case TransmitSettingSelection::kSend:
          oled.printf("%c SEND", marker);
          break;
        case TransmitSettingSelection::kAutoProfile:
          oled.printf("%c SET AUTO PROFILE", marker);
          break;
        case TransmitSettingSelection::kLearn:
          oled.printf("%c LEARN CURRENT", marker);
          break;
        case TransmitSettingSelection::kCount:
          break;
      }
    }
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kLearnCategory) {
    const uint8_t selected = static_cast<uint8_t>(learnCategory);
    const uint8_t categoryCount = static_cast<uint8_t>(LearnCategory::kCount);
    uint8_t first = selected > 1 ? selected - 1 : 0;
    if (first + 4 > categoryCount) {
      first = categoryCount - 4;
    }

    oled.setCursor(0, 0);
    oled.println("IR LEARN TYPE");
    for (uint8_t row = 0; row < 4; ++row) {
      const uint8_t index = first + row;
      oled.setCursor(0, 14 + row * 12);
      oled.printf("%c %s", index == selected ? '>' : ' ',
                  getLearnCategoryName(static_cast<LearnCategory>(index)));
    }
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kLearnTarget) {
    char target[12];
    oled.setCursor(0, 0);
    oled.printf("LEARN %s", getLearnCategoryName(learnCategory));
    oled.setCursor(0, 15);
    oled.printf("TARGET: %s", formatLearnTarget(target, sizeof(target)));
    oled.setCursor(0, 29);
    oled.println(getLearnBaseContext());
    oled.setCursor(0, 43);
    oled.println("ARM, THEN SET TARGET");
    oled.setCursor(0, 56);
    oled.println("CONFIRM:START BACK");
    oled.display();
    return;
  }

  char line[24];

  if (automaticClockValid) {
    if (automaticNetwork.connected) {
      snprintf(line, sizeof(line), "%02u:%02u W:OK %ddBm",
               automaticClock.hour, automaticClock.minute,
               automaticNetwork.rssiDbm);
    } else {
      snprintf(line, sizeof(line), "%02u:%02u W:WAIT", automaticClock.hour,
               automaticClock.minute);
    }
  } else {
    snprintf(line, sizeof(line), "--:-- W:%s",
             automaticNetwork.configured ? "WAIT" : "CFG?");
  }
  oled.setCursor(0, 0);
  oled.println(line);

  if (isnan(temperatureC) || isnan(humidityPercent)) {
    snprintf(line, sizeof(line), "SHT40: --.-C --.-%%");
  } else {
    snprintf(line, sizeof(line), "T:%4.1fC  H:%4.1f%%", temperatureC,
             humidityPercent);
  }
  oled.setCursor(0, 13);
  oled.println(line);

  oled.setCursor(0, 26);
  oled.printf("AUTO:%s", automaticControlStatus);

  switch (appliedAutomaticSettings.triggerMode) {
    case AutomaticTriggerMode::kTimeAndTemperature:
      snprintf(line, sizeof(line), "BOTH %.1fC %02u:%02u",
               appliedAutomaticSettings.onTemperatureC,
               appliedAutomaticSettings.startHour,
               appliedAutomaticSettings.startMinute);
      break;
    case AutomaticTriggerMode::kTimeOnly:
      snprintf(line, sizeof(line), "TIME AT %02u:%02u",
               appliedAutomaticSettings.startHour,
               appliedAutomaticSettings.startMinute);
      break;
    case AutomaticTriggerMode::kTemperatureOnly:
      snprintf(line, sizeof(line), "TEMP OVER %.1fC",
               appliedAutomaticSettings.onTemperatureC);
      break;
  }
  oled.setCursor(0, 39);
  oled.println(line);

  snprintf(line, sizeof(line), "CONF:SET PUSH:MENU");
  oled.setCursor(0, 52);
  oled.println(line);
  oled.display();
}

}  // namespace

void setUiLastAction(const char *action) {
  snprintf(lastAction, sizeof(lastAction), "%s", action);
}

void setUiLearningProgress(const char *label, uint8_t captured,
                           uint8_t required, bool acceptingSignals) {
  snprintf(learningLabel, sizeof(learningLabel), "%s", label ? label : "");
  learningCaptured = captured;
  learningRequired = required;
  learningAcceptingSignals = acceptingSignals;
  learningStartError = false;
  learningStatusVisible = true;
  lastDisplayDrawMs = 0;
}

void showUiLearningStartError(const char *label) {
  snprintf(learningLabel, sizeof(learningLabel), "%s", label ? label : "");
  learningStartError = true;
  learningAcceptingSignals = false;
  learningStatusVisible = true;
  lastDisplayDrawMs = 0;
}

void clearUiLearningStatus() {
  learningStatusVisible = false;
  learningAcceptingSignals = false;
  learningStartError = false;
  lastDisplayDrawMs = 0;
}

const char *getUiLearningRequestLabel() { return learningRequestLabel; }

const char *getUiTransmitRequestLabel() { return transmitRequestLabel; }

UiTransmitSettings getUiTransmitSettings() { return transmitSettings; }

void setUiTransmitResult(bool sent, const char *label) {
  snprintf(transmitFeedback, sizeof(transmitFeedback), "%s",
           sent ? "IR TX: SENT" : "IR TX: NOT LEARNED");
  setUiLastAction(sent ? "TX CUSTOM" : "TX MISSING");
  Serial.printf("Custom IR %s: %s\n", sent ? "sent" : "not learned",
                label ? label : "");
  lastDisplayDrawMs = 0;
}

void setUiAutomaticProfileResult(bool saved, const char *label) {
  snprintf(transmitFeedback, sizeof(transmitFeedback), "%s",
           saved ? "AUTO PROFILE: SAVED" : "AUTO PROFILE: NO IR");
  setUiLastAction(saved ? "PROFILE SAVED" : "PROFILE ERROR");
  Serial.printf("Automatic profile %s: %s\n", saved ? "saved" : "rejected",
                label ? label : "");
  lastDisplayDrawMs = 0;
}

void showUiCustomLearningExists(const char *label) {
  snprintf(customRecordLabel, sizeof(customRecordLabel), "%s",
           label ? label : "");
  customRecordActionVisible = true;
  lastDisplayDrawMs = 0;
}

void setUiCustomEraseResult(bool erased, const char *label) {
  customRecordActionVisible = false;
  customRecordResultVisible = true;
  customRecordEraseSucceeded = erased;
  snprintf(customRecordLabel, sizeof(customRecordLabel), "%s",
           label ? label : "");
  snprintf(transmitFeedback, sizeof(transmitFeedback), "%s",
           erased ? "IR: ERASED" : "IR: ERASE FAILED");
  setUiLastAction(erased ? "IR ERASED" : "ERASE ERROR");
  Serial.printf("Custom IR erase %s: %s\n", erased ? "complete" : "failed",
                label ? label : "");
  lastDisplayDrawMs = 0;
}

float getUiTemperatureC() { return temperatureC; }

void setUiAutomaticControlState(
    const char *status, bool clockValid, const AutomaticControlClock &clock,
    const AutomaticControlSettings &settings,
    const AutomaticNetworkStatus &network) {
  const char *safeStatus = status ? status : "?";
  if (strcmp(automaticControlStatus, safeStatus) == 0 &&
      automaticClockValid == clockValid &&
      automaticClock.year == clock.year &&
      automaticClock.month == clock.month && automaticClock.day == clock.day &&
      automaticClock.hour == clock.hour &&
      automaticClock.minute == clock.minute &&
      automaticClock.second == clock.second &&
      appliedAutomaticSettings.enabled == settings.enabled &&
      appliedAutomaticSettings.startHour == settings.startHour &&
      appliedAutomaticSettings.startMinute == settings.startMinute &&
      appliedAutomaticSettings.onTemperatureC == settings.onTemperatureC &&
      appliedAutomaticSettings.triggerMode == settings.triggerMode &&
      appliedAutomaticSettings.powerSaveEnabled == settings.powerSaveEnabled &&
      appliedAutomaticSettings.wakeIntervalMinutes ==
          settings.wakeIntervalMinutes &&
      automaticNetwork.configured == network.configured &&
      automaticNetwork.connected == network.connected &&
      automaticNetwork.rssiDbm == network.rssiDbm &&
      strcmp(automaticNetwork.ipAddress, network.ipAddress) == 0) {
    return;
  }

  snprintf(automaticControlStatus, sizeof(automaticControlStatus), "%s",
           safeStatus);
  automaticClockValid = clockValid;
  automaticClock = clock;
  appliedAutomaticSettings = settings;
  automaticNetwork = network;
  lastDisplayDrawMs = 0;
}

AutomaticControlSettings getUiAutomaticSettings() {
  return draftAutomaticSettings;
}

void setupUiHardware(bool interactive) {
  uiInteractiveEnabled = interactive;
  lastUiInteractionMs = millis();
  gpio_hold_dis(static_cast<gpio_num_t>(kHeartbeatLedPin));
  gpio_reset_pin(static_cast<gpio_num_t>(kHeartbeatLedPin));
  pinMode(kHeartbeatLedPin, OUTPUT);
  setHeartbeatLed(false);
  lastHeartbeatToggleMs = millis();
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(400000);

  oledReady = interactive && probeI2cAddress(kOledAddress);
  sht40Ready = probeI2cAddress(kSht40Address);
  Serial.printf("I2C OLED 0x%02X: %s\n", kOledAddress,
                interactive ? (oledReady ? "FOUND" : "NOT FOUND")
                            : "SKIPPED");
  Serial.printf("I2C SHT40 0x%02X: %s\n", kSht40Address,
                sht40Ready ? "FOUND" : "NOT FOUND");
  Serial.printf("Heartbeat LED GPIO%u: %s\n", kHeartbeatLedPin,
                interactive ? "500 ms toggle" : "OFF");

  if (sht40Ready) {
    Serial.println("SHT40 initialization start.");
    sht40Ready = sht40.begin(&Wire);
    if (sht40Ready) {
      sht40.setPrecision(SHT4X_HIGH_PRECISION);
      sht40.setHeater(SHT4X_NO_HEATER);
      Serial.println("SHT40 initialization complete.");
    } else {
      Serial.println("SHT40 initialization failed.");
    }
  }

  if (oledReady) {
    Serial.println("OLED initialization start.");
    oledReady = oled.begin(kOledAddress, false);
    if (oledReady) {
      oled.oled_command(SH110X_DISPLAYON);
    }
    Serial.println(oledReady ? "OLED initialization complete."
                             : "OLED initialization failed.");
  }

  if (interactive) {
    pinMode(kEncoderAPin, INPUT_PULLUP);
    pinMode(kEncoderBPin, INPUT_PULLUP);
    encoderPreviousState =
        (static_cast<uint8_t>(digitalRead(kEncoderAPin)) << 1) |
        static_cast<uint8_t>(digitalRead(kEncoderBPin));
    attachInterrupt(digitalPinToInterrupt(kEncoderAPin), handleEncoderEdge,
                    CHANGE);
    attachInterrupt(digitalPinToInterrupt(kEncoderBPin), handleEncoderEdge,
                    CHANGE);

    initializeButton(encoderPush);
    initializeButton(confirmButton);
    initializeButton(backButton);
  }
  lastSensorReadMs = millis() - kSensorIntervalMs;
  drawDisplay(millis());
}

void setUiEncoderDiagnostics(bool enabled) {
  const uint8_t currentState =
      uiInteractiveEnabled
          ? (static_cast<uint8_t>(digitalRead(kEncoderAPin)) << 1) |
                static_cast<uint8_t>(digitalRead(kEncoderBPin))
          : 0;
  portENTER_CRITICAL(&encoderMux);
  encoderDiagnosticsEnabled = enabled;
  encoderQuarterSteps = 0;
  encoderPendingMoves = 0;
  encoderActivityCount = 0;
  encoderDiagnosticHead = 0;
  encoderDiagnosticTail = 0;
  encoderDiagnosticDropped = 0;
  if (uiInteractiveEnabled) {
    encoderPreviousState = currentState;
  }
  portEXIT_CRITICAL(&encoderMux);
  Serial.printf(
      "Encoder diagnostics: %s, initial AB=%u%u, detent=11, min_edges=2\n",
                enabled ? "ON" : "OFF",
                (currentState >> 1) & 1, currentState & 1);
}

bool getUiEncoderDiagnosticsEnabled() { return encoderDiagnosticsEnabled; }

void prepareUiForSleep() {
  if (oledReady) {
    oled.clearDisplay();
    oled.display();
    oled.oled_command(SH110X_DISPLAYOFF);
  }
  setHeartbeatLed(false);
}

uint32_t getUiLastInteractionMs() { return lastUiInteractionMs; }

UiCommand pollUiHardware() {
  const uint32_t nowMs = millis();
  UiCommand command = UiCommand::kNone;
  updateHeartbeat(nowMs);
  if (!uiInteractiveEnabled) {
    readSensor(nowMs);
    return command;
  }
  const int8_t encoderChange = readEncoderChange();
  const bool encoderPushPressed = updateButton(encoderPush, nowMs);
  const bool confirmPressed = updateButton(confirmButton, nowMs);
  const bool backPressed = updateButton(backButton, nowMs);

  if (encoderChange != 0 || encoderPushPressed || confirmPressed ||
      backPressed) {
    lastUiInteractionMs = nowMs;
  }

  if (customRecordActionVisible) {
    if (encoderPushPressed) {
      customRecordActionVisible = false;
      command = UiCommand::kEraseCustomLearning;
    } else if (confirmPressed) {
      customRecordActionVisible = false;
      command = UiCommand::kOverwriteCustomLearning;
    } else if (backPressed) {
      customRecordActionVisible = false;
      snprintf(transmitFeedback, sizeof(transmitFeedback), "IR: UNCHANGED");
      setUiLastAction("CANCEL");
      lastDisplayDrawMs = 0;
    }
    readSensor(nowMs);
    drawDisplay(nowMs);
    return command;
  }

  if (customRecordResultVisible) {
    if (encoderPushPressed || confirmPressed || backPressed) {
      customRecordResultVisible = false;
      lastDisplayDrawMs = 0;
    }
    readSensor(nowMs);
    drawDisplay(nowMs);
    return command;
  }

  if (learningStatusVisible) {
    if (learningStartError &&
        (encoderPushPressed || confirmPressed || backPressed)) {
      clearUiLearningStatus();
      setUiLastAction("LEARN ERR");
    } else if (learningAcceptingSignals && backPressed) {
      command = UiCommand::kCancelLearning;
    } else if (!learningAcceptingSignals) {
      if (encoderPushPressed) {
        clearUiLearningStatus();
        if (currentScreen == UiScreen::kLearnTarget) {
          adjustLearnTarget(1);
        } else if (currentScreen != UiScreen::kTransmitMenu) {
          currentScreen = UiScreen::kMain;
        }
        setUiLastAction("NEXT");
      } else if (confirmPressed) {
        clearUiLearningStatus();
        if (currentScreen != UiScreen::kLearnTarget &&
            currentScreen != UiScreen::kTransmitMenu) {
          currentScreen = UiScreen::kMain;
        }
        setUiLastAction("LEARNED");
      } else if (backPressed) {
        clearUiLearningStatus();
        if (currentScreen == UiScreen::kLearnTarget) {
          currentScreen = UiScreen::kLearnCategory;
        } else if (currentScreen != UiScreen::kTransmitMenu) {
          currentScreen = UiScreen::kMain;
        }
        setUiLastAction("BACK");
      }
    }
    readSensor(nowMs);
    drawDisplay(nowMs);
    return command;
  }

  if (encoderChange != 0) {
    switch (currentScreen) {
      case UiScreen::kMain:
        break;
      case UiScreen::kMainMenu: {
        const int8_t itemCount =
            static_cast<int8_t>(MainMenuSelection::kCount);
        mainMenuSelection = static_cast<MainMenuSelection>(wrapValue(
            static_cast<int8_t>(mainMenuSelection) + encoderChange, 0,
            itemCount - 1));
        break;
      }
      case UiScreen::kTransmitMenu:
        if (transmitSettingEditing) {
          adjustTransmitSetting(encoderChange);
        } else {
          const int8_t itemCount =
              static_cast<int8_t>(TransmitSettingSelection::kCount);
          transmitSettingSelection =
              static_cast<TransmitSettingSelection>(wrapValue(
                  static_cast<int8_t>(transmitSettingSelection) +
                      encoderChange,
                  0, itemCount - 1));
          transmitFeedback[0] = '\0';
        }
        break;
      case UiScreen::kLearnCategory: {
        const int8_t categoryCount =
            static_cast<int8_t>(LearnCategory::kCount);
        const int8_t selected = wrapValue(
            static_cast<int8_t>(learnCategory) + encoderChange, 0,
            categoryCount - 1);
        learnCategory = static_cast<LearnCategory>(selected);
        break;
      }
      case UiScreen::kLearnTarget:
        adjustLearnTarget(encoderChange);
        break;
      case UiScreen::kClock:
        break;
      case UiScreen::kAutomaticSettings:
        if (automaticSettingEditing) {
          adjustAutomaticSetting(encoderChange);
        } else {
          const int8_t itemCount =
              static_cast<int8_t>(AutomaticSettingSelection::kCount);
          automaticSettingSelection =
              static_cast<AutomaticSettingSelection>(wrapValue(
                  static_cast<int8_t>(automaticSettingSelection) +
                      encoderChange,
                  0, itemCount - 1));
        }
        break;
    }
  }

  if (encoderPushPressed) {
    switch (currentScreen) {
      case UiScreen::kMain:
        currentScreen = UiScreen::kMainMenu;
        mainMenuSelection = MainMenuSelection::kTransmit;
        setUiLastAction("MENU");
        break;
      case UiScreen::kMainMenu:
        enterSelectedMainMenu();
        break;
      case UiScreen::kLearnCategory:
        currentScreen = UiScreen::kLearnTarget;
        break;
      case UiScreen::kLearnTarget:
        Serial.println("Button: PUSH (use CONFIRM for action)");
        break;
      case UiScreen::kTransmitMenu:
        command = activateTransmitSetting();
        break;
      case UiScreen::kClock:
        break;
      case UiScreen::kAutomaticSettings:
        command = activateAutomaticSetting();
        break;
    }
  }

  if (confirmPressed) {
    switch (currentScreen) {
      case UiScreen::kMain:
        openAutomaticSettings(UiScreen::kMain);
        setUiLastAction("AUTO SET");
        Serial.println("Button: CONFIRM -> automatic settings");
        break;
      case UiScreen::kMainMenu:
        enterSelectedMainMenu();
        break;
      case UiScreen::kTransmitMenu:
        command = activateTransmitSetting();
        break;
      case UiScreen::kLearnCategory:
        currentScreen = UiScreen::kLearnTarget;
        break;
      case UiScreen::kLearnTarget:
        buildLearningRequestLabel();
        command = UiCommand::kStartLearning;
        Serial.printf("Button: CONFIRM -> learn %s\n", learningRequestLabel);
        break;
      case UiScreen::kClock:
        break;
      case UiScreen::kAutomaticSettings:
        command = activateAutomaticSetting();
        break;
    }
  }

  if (backPressed) {
    switch (currentScreen) {
      case UiScreen::kMain:
        setUiLastAction("BACK");
        break;
      case UiScreen::kMainMenu:
        currentScreen = UiScreen::kMain;
        break;
      case UiScreen::kTransmitMenu:
        if (transmitSettingEditing) {
          transmitSettingEditing = false;
          break;
        }
        currentScreen = UiScreen::kMainMenu;
        break;
      case UiScreen::kLearnCategory:
        currentScreen = UiScreen::kMainMenu;
        break;
      case UiScreen::kLearnTarget:
        currentScreen = UiScreen::kLearnCategory;
        break;
      case UiScreen::kClock:
        currentScreen = UiScreen::kMainMenu;
        break;
      case UiScreen::kAutomaticSettings:
        if (automaticSettingEditing) {
          automaticSettingEditing = false;
        } else {
          draftAutomaticSettings = appliedAutomaticSettings;
          currentScreen = automaticSettingsReturnScreen;
        }
        break;
    }
    setUiLastAction("BACK");
  }

  readSensor(nowMs);
  drawDisplay(nowMs);
  return command;
}
