#pragma once

#include <IRrecv.h>
#include <stdint.h>

constexpr uint8_t kLearningSamplesRequired = 3;

enum class LearningCaptureResult : uint8_t {
  kSaved,
  kComplete,
  kRejected,
  kStorageError,
};

bool setupIrLearning();
bool startIrLearning(const char *label);
void cancelIrLearning();
bool isIrLearningActive();
const char *getIrLearningLabel();
uint8_t getIrLearningSampleCount();

LearningCaptureResult captureIrLearningSample(const decode_results *result);

void listIrLearningRecords();
bool showIrLearningRecord(const char *label);
void exportIrLearningRecords();
bool eraseIrLearningRecord(const char *label);
bool loadIrLearningSample(const char *label, uint16_t *timings,
                          uint16_t capacity, uint16_t *timingCount);
