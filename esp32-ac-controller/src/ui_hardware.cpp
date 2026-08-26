#include "ui_hardware.h"

#include <Adafruit_SH110X.h>
#include <Adafruit_SHT4x.h>
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

namespace {

constexpr uint8_t kSdaPin = 8;
constexpr uint8_t kSclPin = 9;
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
  kStartTime,
  kOnTemperature,
  kSave,
  kCount,
};

enum class TransmitSelection : uint8_t {
  kOn,
  kOff,
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
AutomaticControlSettings appliedAutomaticSettings = {true, 6, 0, 28.0F};
AutomaticControlSettings draftAutomaticSettings = {true, 6, 0, 28.0F};
UiScreen currentScreen = UiScreen::kMain;
MainMenuSelection mainMenuSelection = MainMenuSelection::kTransmit;
AutomaticSettingSelection automaticSettingSelection =
    AutomaticSettingSelection::kEnabled;
bool automaticSettingEditing = false;
uint8_t automaticTimeEditField = 0;
TransmitSelection transmitSelection = TransmitSelection::kOn;
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

uint8_t previousEncoderState = 0;
int8_t encoderQuarterSteps = 0;
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

int8_t readEncoderChange() {
  static const int8_t transitionTable[16] = {
      0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

  const uint8_t currentState =
      (static_cast<uint8_t>(digitalRead(kEncoderAPin)) << 1) |
      static_cast<uint8_t>(digitalRead(kEncoderBPin));
  if (currentState == previousEncoderState) {
    return 0;
  }

  encoderQuarterSteps +=
      transitionTable[(previousEncoderState << 2) | currentState];
  previousEncoderState = currentState;

  int8_t change = 0;
  if (encoderQuarterSteps >= 4) {
    change = 1;
    encoderQuarterSteps = 0;
  } else if (encoderQuarterSteps <= -4) {
    change = -1;
    encoderQuarterSteps = 0;
  }

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

void enterSelectedMainMenu() {
  switch (mainMenuSelection) {
    case MainMenuSelection::kTransmit:
      currentScreen = UiScreen::kTransmitMenu;
      transmitSelection = TransmitSelection::kOn;
      break;
    case MainMenuSelection::kLearn:
      currentScreen = UiScreen::kLearnCategory;
      break;
    case MainMenuSelection::kClock:
      currentScreen = UiScreen::kClock;
      break;
    case MainMenuSelection::kAutomaticSettings:
      currentScreen = UiScreen::kAutomaticSettings;
      automaticSettingSelection = AutomaticSettingSelection::kEnabled;
      automaticSettingEditing = false;
      draftAutomaticSettings = appliedAutomaticSettings;
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
    case AutomaticSettingSelection::kSave:
      currentScreen = UiScreen::kMainMenu;
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
    oled.println("KOREA CLOCK");
    if (automaticClockValid) {
      oled.setCursor(0, 15);
      oled.printf("%04u-%02u-%02u", automaticClock.year,
                  automaticClock.month, automaticClock.day);
      oled.setCursor(0, 29);
      oled.printf("%02u:%02u:%02u KST", automaticClock.hour,
                  automaticClock.minute, automaticClock.second);
      oled.setCursor(0, 43);
      oled.println("NTP: SYNCED");
    } else {
      oled.setCursor(0, 19);
      oled.println("TIME NOT SYNCED");
      oled.setCursor(0, 36);
      oled.printf("STATUS:%s", automaticControlStatus);
    }
    oled.setCursor(0, 56);
    oled.println("BACK: EXIT");
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kAutomaticSettings) {
    const uint8_t selected =
        static_cast<uint8_t>(automaticSettingSelection);
    const char marker = automaticSettingEditing ? '*' : '>';
    oled.setCursor(0, 0);
    oled.println("AUTO SETTINGS");
    oled.setCursor(0, 14);
    oled.printf("%c ENABLE  %s",
                selected == 0 ? marker : ' ',
                draftAutomaticSettings.enabled ? "ON" : "OFF");
    oled.setCursor(0, 26);
    oled.printf("%c START   %02u:%02u",
                selected == 1 ? marker : ' ',
                draftAutomaticSettings.startHour,
                draftAutomaticSettings.startMinute);
    if (selected == 1 && automaticSettingEditing) {
      oled.printf(" %c", automaticTimeEditField == 0 ? 'H' : 'M');
    }
    oled.setCursor(0, 38);
    oled.printf("%c ON TEMP %.1fC",
                selected == 2 ? marker : ' ',
                draftAutomaticSettings.onTemperatureC);
    oled.setCursor(0, 50);
    oled.printf("%c SAVE & EXIT", selected == 3 ? '>' : ' ');
    oled.display();
    return;
  }

  if (currentScreen == UiScreen::kTransmitMenu) {
    oled.setCursor(0, 0);
    oled.println("IR TRANSMIT TEST");
    oled.setCursor(0, 16);
    oled.println(transmitSelection == TransmitSelection::kOn ? "> AC ON" :
                                                               "  AC ON");
    oled.setCursor(0, 29);
    oled.println(transmitSelection == TransmitSelection::kOff ? "> AC OFF" :
                                                                "  AC OFF");
    oled.setCursor(0, 45);
    oled.println("CONFIRM: SEND");
    oled.setCursor(0, 56);
    oled.println("BACK: EXIT");
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

  if (isnan(temperatureC) || isnan(humidityPercent)) {
    snprintf(line, sizeof(line), "SHT40: --.-C --.-%%");
  } else {
    snprintf(line, sizeof(line), "T:%4.1fC  H:%4.1f%%", temperatureC,
             humidityPercent);
  }
  oled.setCursor(0, 0);
  oled.println(line);

  if (automaticClockValid) {
    snprintf(line, sizeof(line), "TIME:%02u:%02u KST", automaticClock.hour,
             automaticClock.minute);
  } else {
    snprintf(line, sizeof(line), "TIME:--:-- KST");
  }
  oled.setCursor(0, 13);
  oled.println(line);

  oled.setCursor(0, 26);
  oled.printf("AUTO:%s", automaticControlStatus);

  snprintf(line, sizeof(line), "ON>%.1f FROM %02u:%02u",
           appliedAutomaticSettings.onTemperatureC,
           appliedAutomaticSettings.startHour,
           appliedAutomaticSettings.startMinute);
  oled.setCursor(0, 39);
  oled.println(line);

  snprintf(line, sizeof(line), "LAST:%s", lastAction);
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

float getUiTemperatureC() { return temperatureC; }

void setUiAutomaticControlState(
    const char *status, bool clockValid, const AutomaticControlClock &clock,
    const AutomaticControlSettings &settings) {
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
      appliedAutomaticSettings.onTemperatureC == settings.onTemperatureC) {
    return;
  }

  snprintf(automaticControlStatus, sizeof(automaticControlStatus), "%s",
           safeStatus);
  automaticClockValid = clockValid;
  automaticClock = clock;
  appliedAutomaticSettings = settings;
  lastDisplayDrawMs = 0;
}

AutomaticControlSettings getUiAutomaticSettings() {
  return draftAutomaticSettings;
}

void setupUiHardware() {
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(100000);

  oledReady = probeI2cAddress(kOledAddress);
  sht40Ready = probeI2cAddress(kSht40Address);
  Serial.printf("I2C OLED 0x%02X: %s\n", kOledAddress,
                oledReady ? "FOUND" : "NOT FOUND");
  Serial.printf("I2C SHT40 0x%02X: %s\n", kSht40Address,
                sht40Ready ? "FOUND" : "NOT FOUND");

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
    Serial.println(oledReady ? "OLED initialization complete."
                             : "OLED initialization failed.");
  }

  pinMode(kEncoderAPin, INPUT_PULLUP);
  pinMode(kEncoderBPin, INPUT_PULLUP);
  previousEncoderState =
      (static_cast<uint8_t>(digitalRead(kEncoderAPin)) << 1) |
      static_cast<uint8_t>(digitalRead(kEncoderBPin));

  initializeButton(encoderPush);
  initializeButton(confirmButton);
  initializeButton(backButton);
  lastSensorReadMs = millis() - kSensorIntervalMs;
  drawDisplay(millis());
}

UiCommand pollUiHardware() {
  const uint32_t nowMs = millis();
  UiCommand command = UiCommand::kNone;
  const int8_t encoderChange = readEncoderChange();
  const bool encoderPushPressed = updateButton(encoderPush, nowMs);
  const bool confirmPressed = updateButton(confirmButton, nowMs);
  const bool backPressed = updateButton(backButton, nowMs);

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
        } else {
          currentScreen = UiScreen::kMain;
        }
        setUiLastAction("NEXT");
      } else if (confirmPressed) {
        clearUiLearningStatus();
        if (currentScreen != UiScreen::kLearnTarget) {
          currentScreen = UiScreen::kMain;
        }
        setUiLastAction("LEARNED");
      } else if (backPressed) {
        clearUiLearningStatus();
        currentScreen = currentScreen == UiScreen::kLearnTarget
                            ? UiScreen::kLearnCategory
                            : UiScreen::kMain;
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
        transmitSelection = transmitSelection == TransmitSelection::kOn
                                ? TransmitSelection::kOff
                                : TransmitSelection::kOn;
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
      case UiScreen::kTransmitMenu:
        Serial.println("Button: PUSH (use CONFIRM for action)");
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
        setUiLastAction("AUTO STATUS");
        Serial.println("Button: CONFIRM -> automatic control status");
        break;
      case UiScreen::kMainMenu:
        enterSelectedMainMenu();
        break;
      case UiScreen::kTransmitMenu:
        command = transmitSelection == TransmitSelection::kOn
                      ? UiCommand::kSendOn
                      : UiCommand::kSendOff;
        Serial.printf("Button: CONFIRM -> send %s\n",
                      command == UiCommand::kSendOn ? "ON" : "OFF");
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
          currentScreen = UiScreen::kMainMenu;
        }
        break;
    }
    setUiLastAction("BACK");
  }

  readSensor(nowMs);
  drawDisplay(nowMs);
  return command;
}
