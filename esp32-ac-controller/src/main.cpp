#include <Arduino.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRac.h>
#include <ctype.h>
#include <string.h>

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

IRrecv irrecv(kIrReceivePin, kCaptureBufferSize, kReceiveTimeoutMs, true);
IRsend irsend(kIrTransmitPin);
decode_results results;

char commandBuffer[kCommandBufferSize];
size_t commandLength = 0;

void printCommandHelp() {
  Serial.println("Commands:");
  Serial.println("  on | off | help");
  Serial.println("  learn <label> | cancel");
  Serial.println("  list | show <label> | export | erase <label>");
}

void sendRawCommand(const char *name, uint16_t timings[], size_t timingCount) {
  Serial.printf("IR TX %s (%u timings)\n", name,
                static_cast<unsigned int>(timingCount));
  irsend.sendRaw(timings, timingCount, kIrCarrierKhz);
  Serial.printf("IR TX %s complete\n", name);
  setUiLastAction(strcmp(name, "on") == 0 ? "TX ON" : "TX OFF");
}

void handleCommand(const char *command) {
  if (strncmp(command, "learn ", 6) == 0) {
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
    sendRawCommand("on", kAutoOnRaw,
                   sizeof(kAutoOnRaw) / sizeof(kAutoOnRaw[0]));
  } else if (strcmp(command, "off") == 0) {
    if (isIrLearningActive()) {
      Serial.println("IR transmit is disabled during learning.");
      return;
    }
    sendRawCommand("off", kAutoOffRaw,
                   sizeof(kAutoOffRaw) / sizeof(kAutoOffRaw[0]));
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
  setupUiHardware();
  Serial.println();
  Serial.println("ESP32-C3 IR receiver/transmitter ready.");
  Serial.println("Receiver: point the AC remote at GPIO3 receiver.");
  printCommandHelp();
}

void loop() {
  pollSerialCommands();
  const UiCommand uiCommand = pollUiHardware();
  if (uiCommand == UiCommand::kSendOn) {
    sendRawCommand("on", kAutoOnRaw,
                   sizeof(kAutoOnRaw) / sizeof(kAutoOnRaw[0]));
  } else if (uiCommand == UiCommand::kSendOff) {
    sendRawCommand("off", kAutoOffRaw,
                   sizeof(kAutoOffRaw) / sizeof(kAutoOffRaw[0]));
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
  }

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
