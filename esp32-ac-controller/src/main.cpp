#include <Arduino.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRac.h>
#include <ctype.h>
#include <string.h>

#include "automatic_control.h"
#include "ir_learning.h"
#include "ir_packets.h"
#include "ui_hardware.h"

// HX-M121 OUT -> GPIO3. Power the receiver from 3.3 V so its OUT level is safe
// for the ESP32-C3 input.
constexpr uint16_t kIrReceivePin = 3;
// HX-53 signal input -> GPIO4.
constexpr uint16_t kIrTransmitPin = 4;

// Air-conditioner remotes often send long, stateful frames. 1024 entries is
// intentionally larger than the default buffer so their raw timings fit.
constexpr uint16_t kCaptureBufferSize = 1024;
// 50 ms ends capture after the usual inter-frame gap, while allowing long frames.
constexpr uint8_t kReceiveTimeoutMs = 50;
constexpr size_t kCommandBufferSize = 64;
constexpr uint16_t kLearnedTimingCapacity = 1200;
constexpr char kAutomaticOnLabel[] = "cool_27_f1_swing_on_turbo_off";
constexpr char kAutomaticOffLabel[] = "power_off";

IRrecv irrecv(kIrReceivePin, kCaptureBufferSize, kReceiveTimeoutMs, true);
IRsend irsend(kIrTransmitPin);
decode_results results;

char commandBuffer[kCommandBufferSize];
size_t commandLength = 0;
uint16_t learnedTimings[kLearnedTimingCapacity];

void printCommandHelp() {
  Serial.println("Commands:");
  Serial.println("  on | off | help");
  Serial.println("  wifi info | wifi scan | wifi detail");
  Serial.println("  learn <label> | cancel");
  Serial.println("  list | show <label> | export | erase <label>");
}

bool sendLearnedCommand(const char *label, const char *displayName,
                        uint16_t fallbackTimings[], size_t fallbackCount) {
  uint16_t timingCount = 0;
  if (loadIrLearningSample(label, learnedTimings, kLearnedTimingCapacity,
                           &timingCount)) {
    Serial.printf("IR TX learned %s (%u timings)\n", label, timingCount);
    irsend.sendRaw(learnedTimings, timingCount, kIrCarrierKhz);
    Serial.printf("IR TX learned %s complete\n", label);
  } else {
    Serial.printf("Using built-in fallback for %s.\n", label);
    irsend.sendRaw(fallbackTimings, fallbackCount, kIrCarrierKhz);
  }
  setUiLastAction(displayName);
  return true;
}

bool sendConfiguredOn(const char *displayName) {
  return sendLearnedCommand(
      kAutomaticOnLabel, displayName, kAutoOnRaw,
      sizeof(kAutoOnRaw) / sizeof(kAutoOnRaw[0]));
}

bool sendConfiguredOff(const char *displayName) {
  return sendLearnedCommand(
      kAutomaticOffLabel, displayName, kAutoOffRaw,
      sizeof(kAutoOffRaw) / sizeof(kAutoOffRaw[0]));
}

void handleCommand(const char *command) {
  if (strcmp(command, "wifi info") == 0) {
    printWifiCredentialDiagnostics();
  } else if (strcmp(command, "wifi scan") == 0) {
    scanWifiNetworks();
  } else if (strcmp(command, "wifi detail") == 0) {
    runWifiDetailedDiagnostics();
  } else if (strncmp(command, "learn ", 6) == 0) {
    const char *label = command + 6;
    if (startIrLearning(label)) {
      setUiLearningProgress(getIrLearningLabel(), 0,
                            kLearningSamplesRequired, true);
    }
  } else if (strcmp(command, "cancel") == 0) {
    cancelIrLearning();
    clearUiLearningStatus();
    setUiLastAction("CANCEL");
  } else if (strcmp(command, "list") == 0) {
    listIrLearningRecords();
  } else if (strncmp(command, "show ", 5) == 0) {
    showIrLearningRecord(command + 5);
  } else if (strcmp(command, "export") == 0) {
    exportIrLearningRecords();
  } else if (strncmp(command, "erase ", 6) == 0) {
    const bool wasActive = isIrLearningActive();
    if (eraseIrLearningRecord(command + 6) && wasActive &&
        !isIrLearningActive()) {
      clearUiLearningStatus();
      setUiLastAction("ERASED");
    }
  } else if (strcmp(command, "on") == 0) {
    if (isIrLearningActive()) {
      Serial.println("IR transmit is disabled during learning.");
      return;
    }
    sendConfiguredOn("TX ON");
  } else if (strcmp(command, "off") == 0) {
    if (isIrLearningActive()) {
      Serial.println("IR transmit is disabled during learning.");
      return;
    }
    sendConfiguredOff("TX OFF");
  } else if (strcmp(command, "help") == 0) {
    printCommandHelp();
  } else {
    Serial.printf("Unknown command: %s\n", command);
    printCommandHelp();
  }
}

void pollSerialCommands() {
  while (Serial.available()) {
    const int received = Serial.read();
    if (received == '\r') {
      continue;
    }

    if (received == '\n') {
      commandBuffer[commandLength] = '\0';
      if (commandLength) {
        handleCommand(commandBuffer);
      }
      commandLength = 0;
      continue;
    }

    if (commandLength < kCommandBufferSize - 1) {
      commandBuffer[commandLength++] =
          static_cast<char>(tolower(static_cast<unsigned char>(received)));
    } else {
      commandLength = 0;
      Serial.println("Command is too long.");
      printCommandHelp();
    }
  }
}

void printDecodedResult(const decode_results *result) {
  Serial.println(resultToHumanReadableBasic(result));

  Serial.printf("Protocol    : %s\n", typeToString(result->decode_type).c_str());
  Serial.printf("Bits        : %u\n", result->bits);
  Serial.print("Value       : 0x");
  serialPrintUint64(result->value, 16);
  Serial.println();

  // Supported AC protocols expose a readable state (power, mode, temperature,
  // fan, etc.). The string is empty when the protocol has no AC-state decoder.
  const String acState = IRAcUtils::resultAcToString(result);
  if (acState.length()) {
    Serial.print("AC state    : ");
    Serial.println(acState);
  }

  // This includes the state/raw representation and, importantly, raw timings
  // even when the protocol is UNKNOWN. Copy the output into an issue/sketch if
  // a protocol needs further analysis.
  Serial.println("Source/raw  :");
  Serial.println(resultToSourceCode(result));
}

void setup() {
  Serial.begin(115200);
  delay(500);  // Lets the USB serial monitor connect after reset.

  irrecv.enableIRIn();
  irsend.begin();
  setupIrLearning();
  printWifiCredentialDiagnostics();
  setupAutomaticControl();
  setupUiHardware();
  const AutomaticControlClock initialClock = {};
  setUiAutomaticControlState(getAutomaticControlStatus(), false, initialClock,
                             getAutomaticControlSettings(),
                             getAutomaticNetworkStatus());
  Serial.println();
  Serial.println("ESP32-C3 IR receiver/transmitter ready.");
  Serial.println("Receiver: point the AC remote at GPIO3 receiver.");
  printCommandHelp();
}

void loop() {
  pollSerialCommands();
  const UiCommand uiCommand = pollUiHardware();
  if (uiCommand == UiCommand::kSendOn) {
    sendConfiguredOn("TX ON");
  } else if (uiCommand == UiCommand::kSendOff) {
    sendConfiguredOff("TX OFF");
  } else if (uiCommand == UiCommand::kStartLearning) {
    const char *label = getUiLearningRequestLabel();
    if (startIrLearning(label)) {
      setUiLearningProgress(getIrLearningLabel(), 0,
                            kLearningSamplesRequired, true);
    } else {
      showUiLearningStartError(label);
      setUiLastAction("LEARN ERR");
    }
  } else if (uiCommand == UiCommand::kCancelLearning) {
    cancelIrLearning();
    clearUiLearningStatus();
    setUiLastAction("CANCEL");
  } else if (uiCommand == UiCommand::kSaveAutomaticSettings) {
    if (saveAutomaticControlSettings(getUiAutomaticSettings())) {
      setUiLastAction("AUTO SAVED");
    } else {
      setUiLastAction("SAVE ERROR");
    }
  }

  const AutomaticControlCommand automaticCommand =
      pollAutomaticControl(getUiTemperatureC());
  if (automaticCommand == AutomaticControlCommand::kSendOn &&
      !isIrLearningActive() && sendConfiguredOn("AUTO ON")) {
    markAutomaticOnSent();
  }

  AutomaticControlClock localClock = {};
  const bool clockValid = getAutomaticControlClock(&localClock);
  setUiAutomaticControlState(getAutomaticControlStatus(), clockValid,
                             localClock, getAutomaticControlSettings(),
                             getAutomaticNetworkStatus());

  if (irrecv.decode(&results)) {
    Serial.println("\n========== IR received ==========");
    if (isIrLearningActive()) {
      Serial.println(resultToHumanReadableBasic(&results));
      const LearningCaptureResult captureResult =
          captureIrLearningSample(&results);
      if (captureResult == LearningCaptureResult::kSaved ||
          captureResult == LearningCaptureResult::kComplete) {
        setUiLearningProgress(
            getIrLearningLabel(), getIrLearningSampleCount(),
            kLearningSamplesRequired,
            captureResult != LearningCaptureResult::kComplete);
      }
    } else {
      printDecodedResult(&results);
      setUiLastAction("IR RX");
    }
    irrecv.resume();  // Re-arm receiver for the next packet.
  }
}
