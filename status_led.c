// status_led.c
#include "pico/cyw43_arch.h"
#include "status_led.h"

void status_led_set(bool on)
{
    static int8_t last = -1;          // -1 = jamais écrit
    if (last == (int8_t)on) return;   // rien à faire : pas de transaction SPI
    last = (int8_t)on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}