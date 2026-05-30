#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { TOUCH_LEFT, TOUCH_MIDDLE, TOUCH_RIGHT } touch_pad_id_t;
typedef void (*touch_callback_t)(touch_pad_id_t pad);
/* Combo callback: fired once when the LEFT and RIGHT pads are held together
 * for ~15 s.  Used to summon the WiFi setup AP on demand. */
typedef void (*touch_combo_callback_t)(void);
void touch_input_init(void);
void touch_input_register_callback(touch_callback_t cb);
void touch_input_register_combo_callback(touch_combo_callback_t cb);
#ifdef __cplusplus
}
#endif
