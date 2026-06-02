#ifndef MAIN_UI_H
#define MAIN_UI_H

#include <stddef.h>
#include <stdint.h>
#include "../ui_bridge.h"
#include "esp_lv_adapter.h"
#include "alarm_pomodoro.h"
#include "alarm_sleep_24h.h"
#include "alarm_end.h"

#define SCREEN_WIDTH    360
#define SCREEN_HEIGHT   360

#ifdef __cplusplus
extern "C" {
#endif

// Font declarations
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_montserrat_40);

// Image declarations
LV_IMG_DECLARE(time_start);
LV_IMG_DECLARE(time_end);
LV_IMG_DECLARE(time_sleep);
LV_IMG_DECLARE(time_arc_texture);
LV_IMG_DECLARE(clock_loop);
LV_IMG_DECLARE(watch_bg);
extern const uint8_t clock_loop_eaf[699139];

/**
 * @brief Switch to specified page (unified page switching interface)
 *
 * This function handles all page visibility and state management in one call.
 * Use this instead of separate hide/show functions.
 *
 * @param page_name Target page name to switch to (e.g., "DUMMY", "POMODORO", "SLEEP", "PAGE_TIME_UP")
 */
void main_ui_switch_page(const char *page_name);

#ifdef __cplusplus
}
#endif

#endif // MAIN_UI_H
