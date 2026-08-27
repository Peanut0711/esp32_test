#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr gpio_num_t kWakePin = GPIO_NUM_9;
constexpr gpio_num_t kHeartbeatLedPin = GPIO_NUM_8;
constexpr uint32_t kPreSleepDelayMs = 30000;
constexpr uint64_t kTimerFallbackUs = 15ULL * 1000ULL * 1000ULL;

void setLed(bool enabled) {
  gpio_set_level(kHeartbeatLedPin, enabled ? 0 : 1);
}

void blinkForever(uint32_t intervalMs) {
  bool ledOn = false;
  while (true) {
    ledOn = !ledOn;
    setLed(ledOn);
    vTaskDelay(pdMS_TO_TICKS(intervalMs));
  }
}

}  // namespace

extern "C" void app_main() {
  gpio_config_t ledConfig{};
  ledConfig.pin_bit_mask = 1ULL << kHeartbeatLedPin;
  ledConfig.mode = GPIO_MODE_OUTPUT;
  ledConfig.pull_up_en = GPIO_PULLUP_DISABLE;
  ledConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ledConfig.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&ledConfig));
  setLed(false);

  gpio_config_t wakeConfig{};
  wakeConfig.pin_bit_mask = 1ULL << kWakePin;
  wakeConfig.mode = GPIO_MODE_INPUT;
  wakeConfig.pull_up_en = GPIO_PULLUP_ENABLE;
  wakeConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  wakeConfig.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&wakeConfig));

  // A 500 ms heartbeat for 30 seconds marks the pre-sleep period.
  for (uint32_t elapsedMs = 0; elapsedMs < kPreSleepDelayMs;
       elapsedMs += 500) {
    setLed((elapsedMs / 500) % 2 == 0);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  setLed(false);

  // Do not enter sleep while BOOT is already held low.
  while (gpio_get_level(kWakePin) == 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  ESP_ERROR_CHECK(gpio_wakeup_enable(kWakePin, GPIO_INTR_LOW_LEVEL));
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(kTimerFallbackUs));

  const esp_err_t sleepResult = esp_light_sleep_start();
  if (sleepResult != ESP_OK) {
    // Very rapid blinking means light sleep could not start.
    blinkForever(50);
  }

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_GPIO) {
    // Fast blinking means the BOOT/GPIO9 wake path worked.
    blinkForever(100);
  }
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    // Slow blinking means sleep worked but GPIO9 did not wake it in time.
    blinkForever(1000);
  }

  // Medium-speed blinking means an unexpected wake cause.
  blinkForever(300);
}
