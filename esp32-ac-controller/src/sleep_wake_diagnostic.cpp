#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {

// The UI module's BACK button pulls GPIO1 low.
constexpr gpio_num_t kWakePin = GPIO_NUM_1;
constexpr uint8_t kHeartbeatLedPin = 8;
constexpr uint32_t kHeartbeatToggleIntervalMs = 500;
constexpr uint32_t kWakeSuccessToggleIntervalMs = 100;
constexpr uint32_t kPreSleepDelayMs = 30000;

bool heartbeatLedOn = false;
uint32_t lastHeartbeatToggleMs = 0;

void setHeartbeatLed(bool enabled) {
  heartbeatLedOn = enabled;
  digitalWrite(kHeartbeatLedPin, enabled ? LOW : HIGH);
}

}  // namespace

void setup() {
  pinMode(kHeartbeatLedPin, OUTPUT);
  setHeartbeatLed(false);

  Serial.begin(115200);
  delay(500);
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  Serial.printf("Minimal deep-sleep wake diagnostic: GPIO%d -> GND\n",
                static_cast<int>(kWakePin));
  Serial.printf("Boot wakeup cause: %d%s\n", static_cast<int>(wakeupCause),
                wakeupCause == ESP_SLEEP_WAKEUP_GPIO ? " (GPIO)" : "");

  if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("GPIO1 deep-sleep wake succeeded; fast blink enabled.");
    Serial.flush();
    lastHeartbeatToggleMs = millis();
    return;
  }

  pinMode(static_cast<uint8_t>(kWakePin), INPUT_PULLUP);

  Serial.printf("Deep sleep starts in %lu seconds.\n",
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

  const esp_err_t wakeResult = esp_deep_sleep_enable_gpio_wakeup(
      1ULL << static_cast<uint8_t>(kWakePin), ESP_GPIO_WAKEUP_GPIO_LOW);
  Serial.printf("GPIO wake configuration: %s\n", esp_err_to_name(wakeResult));
  Serial.printf("GPIO%d level before sleep: %s\n", static_cast<int>(kWakePin),
                digitalRead(static_cast<uint8_t>(kWakePin)) == HIGH ? "HIGH"
                                                                    : "LOW");
  Serial.println("Entering deep sleep now.");
  Serial.flush();
  delay(50);

  esp_deep_sleep_start();
}

void loop() {
  const uint32_t nowMs = millis();
  if (nowMs - lastHeartbeatToggleMs >= kWakeSuccessToggleIntervalMs) {
    lastHeartbeatToggleMs = nowMs;
    setHeartbeatLed(!heartbeatLedOn);
  }
}
