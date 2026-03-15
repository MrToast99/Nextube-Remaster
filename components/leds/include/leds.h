#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void leds_init(void);
void leds_task_start(void);      /* start the FreeRTOS effect task */
void leds_set_color(int idx, uint8_t r, uint8_t g, uint8_t b);
void leds_set_all(uint8_t r, uint8_t g, uint8_t b);
void leds_set_brightness(uint8_t pct);
void leds_update(void);
void leds_effect_breath(void);
void leds_effect_rainbow(void);
void leds_off(void);
#ifdef __cplusplus
}
#endif
