#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {

// The ESP32-C3 SuperMini onboard BOOT button pulls GPIO9 low.
constexpr gpio_num_t kWakePin = GPIO_NUM_9;
constexpr uint8_t kHeartbeatLedPin = 8;
constexpr uint32_t kHeartbeatToggleIntervalMs = 500;
constexpr uint32_t kPreSleepDelayMs = 30000;

bool heartbeatLedOn = false;
uint32_t lastHeartbeatToggleMs = 0;

void setHeartbeatLed(bool enabled) {
  heartbeatLedOn = enabled;
  digitalWrite(kHeartbeatLedPin, enabled ? LOW : HIGH);
}

}  // namespace

void setup() {
  pinMode(static_cast<uint8_t>(kWakePin), INPUT_PULLUP);
  pinMode(kHeartbeatLedPin, OUTPUT);
  setHeartbeatLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.printf("Minimal light-sleep wake diagnostic: GPIO%d -> GND\n",
                static_cast<int>(kWakePin));

  Serial.printf("Light sleep starts in %lu seconds.\n",
                static_cast<unsigned long>(kPreSleepDelayMs / 1000));
  const uint32_t countdownStartedMs = millis();
  while (millis() - countdownStartedMs < kPreSleepDelayMs) {
    const uint32_t nowMs = millis();
    if (nowMs - lastHeartbeatToggleMs >= kHeartbeatToggleIntervalMs) {
      lastHeartbeatToggleMs = nowMs;
      setHeartbeatLed(!heartbeatLedOn);
    }
    delay(10);
  }
  setHeartbeatLed(false);

  while (digitalRead(static_cast<uint8_t>(kWakePin)) == LOW) {
    delay(10);
  }

  esp_err_t wakeResult =
      gpio_wakeup_enable(kWakePin, GPIO_INTR_LOW_LEVEL);
  if (wakeResult == ESP_OK) {
    wakeResult = esp_sleep_enable_gpio_wakeup();
  }
  Serial.printf("GPIO wake configuration: %s\n", esp_err_to_name(wakeResult));
  Serial.println("Entering light sleep now.");
  Serial.flush();
  delay(50);

  const esp_err_t sleepResult = esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  Serial.printf("Light sleep exited: %s; wakeup cause: %d%s\n",
                esp_err_to_name(sleepResult), static_cast<int>(wakeupCause),
                wakeupCause == ESP_SLEEP_WAKEUP_GPIO ? " (GPIO)" : "");

  setHeartbeatLed(true);
  delay(1000);
  lastHeartbeatToggleMs = millis();
}

void loop() {
  const uint32_t nowMs = millis();
  if (nowMs - lastHeartbeatToggleMs >= kHeartbeatToggleIntervalMs) {
    lastHeartbeatToggleMs = nowMs;
    setHeartbeatLed(!heartbeatLedOn);
  }
}
