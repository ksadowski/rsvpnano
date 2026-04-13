#include "board/BoardConfig.h"

#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace BoardConfig {

void begin() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(300000);
  Wire.setTimeOut(10);
}

void lightSleepUntilBootButton() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  gpio_wakeup_enable(static_cast<gpio_num_t>(PIN_BOOT_BUTTON), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();
  gpio_wakeup_disable(static_cast<gpio_num_t>(PIN_BOOT_BUTTON));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

}  // namespace BoardConfig
