#include "ui_hardware.h"

#include <Adafruit_SH110X.h>
#include <Adafruit_SHT4x.h>
#include <Arduino.h>
#include <Wire.h>

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
constexpr uint32_t kDisplayIntervalMs = 100;

enum class UiScreen : uint8_t {
  kMain,
  kTransmitMenu,
};

enum class TransmitSelection : uint8_t {
  kOn,
  kOff,
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
int8_t targetTemperatureC = 27;
char lastAction[16] = "BOOT";
UiScreen currentScreen = UiScreen::kMain;
TransmitSelection transmitSelection = TransmitSelection::kOn;

uint8_t previousEncoderState = 0;
int8_t encoderQuarterSteps = 0;
uint32_t lastSensorReadMs = 0;
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
    Serial.printf("SHT40: %.2f C, %.2f %%RH\n", temperatureC,
                  humidityPercent);
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

  char line[24];

  if (isnan(temperatureC) || isnan(humidityPercent)) {
    snprintf(line, sizeof(line), "SHT40: --.-C --.-%%");
  } else {
    snprintf(line, sizeof(line), "T:%4.1fC  H:%4.1f%%", temperatureC,
             humidityPercent);
  }
  oled.setCursor(0, 0);
  oled.println(line);

  snprintf(line, sizeof(line), "SET:%dC  AUTO:OFF", targetTemperatureC);
  oled.setCursor(0, 13);
  oled.println(line);
  oled.setCursor(0, 26);
  oled.println("COOL  F1  SWING ON");

  snprintf(line, sizeof(line), "LAST:%s", lastAction);
  oled.setCursor(0, 39);
  oled.println(line);

  snprintf(line, sizeof(line), "OLED:%s SHT:%s", oledReady ? "OK" : "--",
           sht40Ready ? "OK" : "--");
  oled.setCursor(0, 52);
  oled.println(line);
  oled.display();
}

}  // namespace

void setUiLastAction(const char *action) {
  snprintf(lastAction, sizeof(lastAction), "%s", action);
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
  if (encoderChange != 0) {
    if (currentScreen == UiScreen::kMain) {
      targetTemperatureC =
          constrain(targetTemperatureC + encoderChange, 16, 30);
      snprintf(lastAction, sizeof(lastAction), "ENC %c",
               encoderChange > 0 ? '+' : '-');
      Serial.printf("Encoder: target=%d C\n", targetTemperatureC);
    } else {
      transmitSelection = transmitSelection == TransmitSelection::kOn
                              ? TransmitSelection::kOff
                              : TransmitSelection::kOn;
      Serial.printf("IR menu selection: %s\n",
                    transmitSelection == TransmitSelection::kOn ? "ON" :
                                                                  "OFF");
    }
  }

  if (updateButton(encoderPush, nowMs)) {
    if (currentScreen == UiScreen::kMain) {
      currentScreen = UiScreen::kTransmitMenu;
      transmitSelection = TransmitSelection::kOn;
      setUiLastAction("MENU");
      Serial.println("Button: PUSH -> IR transmit menu");
    } else {
      Serial.println("Button: PUSH (use CONFIRM to send)");
    }
  }
  if (updateButton(confirmButton, nowMs)) {
    if (currentScreen == UiScreen::kTransmitMenu) {
      command = transmitSelection == TransmitSelection::kOn
                    ? UiCommand::kSendOn
                    : UiCommand::kSendOff;
      Serial.printf("Button: CONFIRM -> send %s\n",
                    command == UiCommand::kSendOn ? "ON" : "OFF");
    } else {
      setUiLastAction("AUTO OFF");
      Serial.println("Button: CONFIRM -> automatic control is not enabled");
    }
  }
  if (updateButton(backButton, nowMs)) {
    if (currentScreen == UiScreen::kTransmitMenu) {
      currentScreen = UiScreen::kMain;
      setUiLastAction("BACK");
      Serial.println("Button: BACK -> main screen");
    } else {
      setUiLastAction("BACK");
      Serial.println("Button: BACK");
    }
  }

  readSensor(nowMs);
  drawDisplay(nowMs);
  return command;
}
