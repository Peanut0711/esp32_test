#include "ir_learning.h"

#include <Arduino.h>
#include <FS.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <ctype.h>
#include <string.h>

namespace {

constexpr char kFileMagic[4] = {'I', 'R', 'L', '1'};
constexpr uint8_t kFileVersion = 1;
constexpr size_t kLabelCapacity = 32;
constexpr uint16_t kMinimumTimingCount = 100;
constexpr uint16_t kMaximumTimingCount = 1200;

struct __attribute__((packed)) LearningFileHeader {
  char magic[4];
  uint8_t version;
  char label[kLabelCapacity];
};

struct __attribute__((packed)) LearningSampleHeader {
  uint16_t timingCount;
  uint16_t bits;
  int16_t decodeType;
  uint64_t value;
};

bool storageReady = false;
bool learningActive = false;
char activeLabel[kLabelCapacity] = "";
char activePath[64] = "";
uint8_t activeSampleCount = 0;
uint16_t expectedTimingCount = 0;
uint16_t expectedBits = 0;
int16_t expectedDecodeType = 0;
uint64_t expectedValue = 0;

bool isValidLabel(const char *label) {
  if (!label || !label[0] || strlen(label) >= kLabelCapacity) {
    return false;
  }

  for (const char *cursor = label; *cursor; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (!isalnum(value) && value != '_' && value != '-') {
      return false;
    }
  }
  return true;
}

bool makeRecordPath(const char *label, char *path, size_t pathSize) {
  if (!isValidLabel(label)) {
    return false;
  }
  return snprintf(path, pathSize, "/learn_%s.irl", label) > 0;
}

bool readExact(File &file, void *buffer, size_t length) {
  return file.read(static_cast<uint8_t *>(buffer), length) == length;
}

bool writeExact(File &file, const void *buffer, size_t length) {
  return file.write(static_cast<const uint8_t *>(buffer), length) == length;
}

bool readFileHeader(File &file, LearningFileHeader *header) {
  if (!readExact(file, header, sizeof(*header))) {
    return false;
  }
  return memcmp(header->magic, kFileMagic, sizeof(kFileMagic)) == 0 &&
         header->version == kFileVersion &&
         header->label[kLabelCapacity - 1] == '\0';
}

uint8_t countSamples(File &file) {
  uint8_t count = 0;
  while (file.available()) {
    LearningSampleHeader sample;
    if (!readExact(file, &sample, sizeof(sample)) ||
        sample.timingCount < kMinimumTimingCount ||
        sample.timingCount > kMaximumTimingCount) {
      return 0;
    }

    const size_t nextPosition =
        file.position() + static_cast<size_t>(sample.timingCount) * sizeof(uint16_t);
    if (nextPosition > file.size() || !file.seek(nextPosition)) {
      return 0;
    }
    ++count;
  }
  return count;
}

void printSampleArray(const char *label, uint8_t sampleNumber,
                      const LearningSampleHeader &sample, File &file) {
  char identifier[kLabelCapacity];
  snprintf(identifier, sizeof(identifier), "%s", label);
  for (char *cursor = identifier; *cursor; ++cursor) {
    if (*cursor == '-') {
      *cursor = '_';
    }
  }

  Serial.printf("// %s sample %u, protocol=%s, bits=%u, value=0x",
                label, sampleNumber,
                typeToString(static_cast<decode_type_t>(sample.decodeType)).c_str(),
                sample.bits);
  serialPrintUint64(sample.value, 16);
  Serial.println();
  Serial.printf("uint16_t %s_sample_%u[%u] = {", identifier, sampleNumber,
                sample.timingCount);

  for (uint16_t index = 0; index < sample.timingCount; ++index) {
    uint16_t timing = 0;
    if (!readExact(file, &timing, sizeof(timing))) {
      Serial.println("\n// ERROR: truncated sample");
      return;
    }
    if (index) {
      Serial.print(", ");
    }
    Serial.print(timing);
  }
  Serial.println("};");
}

bool printRecord(const char *path) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }

  LearningFileHeader header;
  if (!readFileHeader(file, &header)) {
    Serial.printf("Invalid learning file: %s\n", path);
    file.close();
    return false;
  }

  Serial.printf("\n===== LEARNED: %s =====\n", header.label);
  uint8_t sampleNumber = 0;
  while (file.available()) {
    LearningSampleHeader sample;
    if (!readExact(file, &sample, sizeof(sample)) ||
        sample.timingCount < kMinimumTimingCount ||
        sample.timingCount > kMaximumTimingCount) {
      Serial.println("Invalid or truncated sample header.");
      file.close();
      return false;
    }
    printSampleArray(header.label, ++sampleNumber, sample, file);
  }
  file.close();
  return true;
}

bool isLearningFileName(const char *name) {
  return name && strstr(name, "learn_") != nullptr &&
         strstr(name, ".irl") != nullptr;
}

}  // namespace

bool setupIrLearning() {
  storageReady = LittleFS.begin(true);
  Serial.printf("LittleFS learning storage: %s\n",
                storageReady ? "READY" : "FAILED");
  return storageReady;
}

bool startIrLearning(const char *label) {
  if (!storageReady) {
    Serial.println("Learning storage is unavailable.");
    return false;
  }
  if (learningActive) {
    Serial.printf("Learning is already active: %s. Use cancel first.\n",
                  activeLabel);
    return false;
  }
  if (!makeRecordPath(label, activePath, sizeof(activePath))) {
    Serial.println("Invalid label. Use 1-31 letters, numbers, '_' or '-'.");
    return false;
  }
  if (LittleFS.exists(activePath)) {
    Serial.printf("Learning record already exists: %s. Erase it first.\n",
                  label);
    return false;
  }

  File file = LittleFS.open(activePath, FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to create learning file: %s\n", activePath);
    return false;
  }

  LearningFileHeader header = {};
  memcpy(header.magic, kFileMagic, sizeof(kFileMagic));
  header.version = kFileVersion;
  snprintf(header.label, sizeof(header.label), "%s", label);
  const bool written = writeExact(file, &header, sizeof(header));
  file.close();
  if (!written) {
    LittleFS.remove(activePath);
    Serial.println("Failed to initialize learning file.");
    return false;
  }

  snprintf(activeLabel, sizeof(activeLabel), "%s", label);
  activeSampleCount = 0;
  expectedTimingCount = 0;
  expectedBits = 0;
  expectedDecodeType = 0;
  expectedValue = 0;
  learningActive = true;
  Serial.printf("Learning started: %s (0/%u)\n", activeLabel,
                kLearningSamplesRequired);
  return true;
}

void cancelIrLearning() {
  if (!learningActive) {
    Serial.println("No learning session is active.");
    return;
  }
  LittleFS.remove(activePath);
  learningActive = false;
  Serial.printf("Learning cancelled: %s\n", activeLabel);
}

bool isIrLearningActive() { return learningActive; }

const char *getIrLearningLabel() { return activeLabel; }

uint8_t getIrLearningSampleCount() { return activeSampleCount; }

LearningCaptureResult captureIrLearningSample(const decode_results *result) {
  if (!learningActive || !result) {
    return LearningCaptureResult::kRejected;
  }

  const uint16_t timingCount = getCorrectedRawLength(result);
  if (timingCount < kMinimumTimingCount || timingCount > kMaximumTimingCount) {
    Serial.printf("Learning rejected: unexpected timing count %u.\n",
                  timingCount);
    return LearningCaptureResult::kRejected;
  }

  const int16_t decodeType = static_cast<int16_t>(result->decode_type);
  if (activeSampleCount == 0) {
    expectedTimingCount = timingCount;
    expectedBits = result->bits;
    expectedDecodeType = decodeType;
    expectedValue = result->value;
  } else if (timingCount != expectedTimingCount || result->bits != expectedBits ||
             decodeType != expectedDecodeType || result->value != expectedValue) {
    Serial.println("Learning rejected: signal differs from the first sample.");
    return LearningCaptureResult::kRejected;
  }

  uint16_t *raw = resultToRawArray(result);
  if (!raw) {
    Serial.println("Learning failed: raw conversion allocation failed.");
    return LearningCaptureResult::kStorageError;
  }

  File file = LittleFS.open(activePath, FILE_APPEND);
  LearningSampleHeader sample = {timingCount, result->bits, decodeType,
                                 result->value};
  bool written = file && writeExact(file, &sample, sizeof(sample)) &&
                 writeExact(file, raw,
                            static_cast<size_t>(timingCount) * sizeof(uint16_t));
  if (file) {
    file.close();
  }
  delete[] raw;

  if (!written) {
    Serial.println("Learning failed while writing LittleFS.");
    return LearningCaptureResult::kStorageError;
  }

  ++activeSampleCount;
  Serial.printf("Learning sample saved: %s (%u/%u)\n", activeLabel,
                activeSampleCount, kLearningSamplesRequired);
  if (activeSampleCount >= kLearningSamplesRequired) {
    learningActive = false;
    Serial.printf("Learning complete: %s\n", activeLabel);
    return LearningCaptureResult::kComplete;
  }
  return LearningCaptureResult::kSaved;
}

void listIrLearningRecords() {
  if (!storageReady) {
    Serial.println("Learning storage is unavailable.");
    return;
  }

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  uint16_t recordCount = 0;
  Serial.println("Learned records:");
  while (file) {
    if (!file.isDirectory() && isLearningFileName(file.name())) {
      LearningFileHeader header;
      if (readFileHeader(file, &header)) {
        const uint8_t samples = countSamples(file);
        Serial.printf("  %s: %u/%u samples\n", header.label, samples,
                      kLearningSamplesRequired);
        ++recordCount;
      }
    }
    file = root.openNextFile();
  }
  if (!recordCount) {
    Serial.println("  (none)");
  }
}

bool showIrLearningRecord(const char *label) {
  char path[64];
  if (!storageReady || !makeRecordPath(label, path, sizeof(path)) ||
      !LittleFS.exists(path)) {
    Serial.printf("Learning record not found: %s\n", label ? label : "");
    return false;
  }
  return printRecord(path);
}

void exportIrLearningRecords() {
  if (!storageReady) {
    Serial.println("Learning storage is unavailable.");
    return;
  }

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  uint16_t recordCount = 0;
  Serial.println("\n========== LEARNING EXPORT ==========");
  while (file) {
    const String path = file.name();
    const bool shouldPrint = !file.isDirectory() && isLearningFileName(path.c_str());
    file.close();
    if (shouldPrint && printRecord(path.c_str())) {
      ++recordCount;
    }
    file = root.openNextFile();
  }
  if (!recordCount) {
    Serial.println("(no learned records)");
  }
  Serial.println("========== END EXPORT ==========");
}

bool eraseIrLearningRecord(const char *label) {
  char path[64];
  if (!storageReady || !makeRecordPath(label, path, sizeof(path)) ||
      !LittleFS.exists(path)) {
    Serial.printf("Learning record not found: %s\n", label ? label : "");
    return false;
  }

  if (learningActive && strcmp(label, activeLabel) == 0) {
    learningActive = false;
  }
  const bool removed = LittleFS.remove(path);
  Serial.printf("Learning record %s: %s\n", label,
                removed ? "erased" : "erase failed");
  return removed;
}
