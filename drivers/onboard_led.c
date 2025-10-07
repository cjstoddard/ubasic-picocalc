//
//  LED Driver for the Raspberry Pi Pico
//
//  This driver controls the onboard LED on the Raspberry Pi Pico.
//  For Pico W, it uses CYW43 driver. For regular Pico, it uses GPIO.
//

#include "pico/stdlib.h"

#ifdef PICO_DEFAULT_LED_PIN
// Regular Pico with GPIO LED
#include "hardware/gpio.h"
#define LED_PIN PICO_DEFAULT_LED_PIN
#define USE_GPIO_LED
#elif defined(CYW43_WL_GPIO_LED_PIN)
// Pico W with CYW43 WiFi chip LED
#include "pico/cyw43_arch.h"
#define USE_CYW43_LED
#else
// No LED available
#define NO_LED
#endif

#include "onboard_led.h"

static bool led_initialised = false;

// Set the state of the on-board LED
void led_set(bool led) {
#ifdef USE_GPIO_LED
    gpio_put(LED_PIN, led);
#elif defined(USE_CYW43_LED)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
#endif
}

// Initialize the LED driver
int led_init() {
    if (led_initialised) {
        return PICO_OK;
    }

#ifdef USE_GPIO_LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    led_initialised = true;
    return PICO_OK;
#elif defined(USE_CYW43_LED)
    if (cyw43_arch_init()) {
        return PICO_ERROR_GENERIC;
    }
    led_initialised = true;
    return PICO_OK;
#else
    // No LED available, but return success anyway
    led_initialised = true;
    return PICO_OK;
#endif
}
