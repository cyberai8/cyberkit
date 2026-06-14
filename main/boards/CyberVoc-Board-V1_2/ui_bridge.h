#ifndef LVGL_DISPLAY_BRIDGE_H
#define LVGL_DISPLAY_BRIDGE_H

#include "lvgl.h"
#include "config.h"

/**
 * @file ui_bridge.h
 * @brief Bridge between LVGL and xiaozhi display module
 *
 * This module provides a unified interface for managing LVGL pages and gestures,
 * making it easy for users to add their own third-party UIs.
 */

/* Page name constants */
#define UI_BRIDGE_PAGE_HOME                      "DUMMY"  /* Base emote display page */
#define UI_BRIDGE_PAGE_WIFI_CONFIG               "WIFI_CONFIG" /* WiFi configuration page */
#define UI_BRIDGE_PAGE_WIFI_SUCCESS              "WIFI_SUCCESS" /* WiFi configuration success page */

/* Gesture detection constants */
#define UI_BRIDGE_GESTURE_LONG_PRESS_TIME_MS     500      /* Long press duration in milliseconds */
#define UI_BRIDGE_GESTURE_SWIPE_THRESHOLD        80       /* Minimum distance for swipe detection */

/* Gesture start position validation constants */
#define UI_BRIDGE_EDGE_THRESHOLD                 30       /* Distance from edge to be considered edge region */
#define UI_BRIDGE_CENTER_RANGE                   50       /* Range around center (±50 pixels) */
#define UI_BRIDGE_CENTER_X                       (DISPLAY_WIDTH / 2)   /* Center X coordinate */
#define UI_BRIDGE_CENTER_Y                       (DISPLAY_HEIGHT / 2)  /* Center Y coordinate */

#ifdef __cplusplus
class Display;
extern "C" {
#endif

/**
 * @brief Gesture event types
 */
typedef enum {
    UI_BRIDGE_GESTURE_NONE = 0,
    UI_BRIDGE_GESTURE_SWIPE_LEFT,
    UI_BRIDGE_GESTURE_SWIPE_RIGHT,
    UI_BRIDGE_GESTURE_SWIPE_UP,
    UI_BRIDGE_GESTURE_SWIPE_DOWN,
    UI_BRIDGE_GESTURE_SHORT_PRESS,
    UI_BRIDGE_GESTURE_LONG_PRESS,
} ui_bridge_gesture_type_t;

/**
 * @brief Page switch callback function type
 *
 * When a gesture navigation occurs, this callback is called with the target page ID.
 * If callback returns true, the default page switching is skipped.
 *
 * @param target_page Target page ID to switch to
 * @param user_data User data passed when registering the callback
 * @return true if handled (skip default switch), false to use default switch
 */
typedef bool (*ui_bridge_page_switch_cb_t)(const char *target_page, void *user_data);

/**
 * @brief Initialize LVGL display bridge
 *
 * This function initializes the bridge between LVGL and xiaozhi display module.
 * It creates the base emote container and sets up page management.
 *
 * @param display Pointer to the Display instance created by the board
 */
#ifdef __cplusplus
void ui_bridge_init(Display *display);
#else
void ui_bridge_init(void *display);
#endif

/**
 * @brief Attach gesture event handler to input device
 *
 * Attaches gesture detection and page navigation handler to the LVGL input device.
 *
 * @param indev Pointer to the LVGL input device
 */
void ui_bridge_attach_gesture_handler(lv_indev_t *indev);

/**
 * @brief Suppress touch/head-triggered actions briefly after a gesture.
 *
 * This prevents a swipe release or nearby head-touch event from immediately
 * starting a voice interaction while the UI/audio state is still settling.
 *
 * @param duration_ms Suppression duration in milliseconds
 */
void ui_bridge_suppress_interactions(uint32_t duration_ms);

/**
 * @brief Return whether touch/head-triggered actions should currently be ignored.
 */
bool ui_bridge_interactions_suppressed(void);

/**
 * @brief Register a page container for page switching
 *
 * Registers a page container that can be switched to via gesture navigation.
 * By default, the page will be included in cycle navigation.
 *
 * @param page_id Unique page identifier (e.g., "DUMMY", "POMODORO", "SLEEP", "PAGE_TIME_UP")
 * @param container Pointer to pointer of the container object
 * @return true if registration successful, false otherwise
 */
bool ui_bridge_register_page(const char *page_id, lv_obj_t **container);

/**
 * @brief Register a page container for page switching with cycle control
 *
 * Registers a page container that can be switched to via gesture navigation.
 *
 * @param page_id Unique page identifier (e.g., "DUMMY", "POMODORO", "SLEEP", "PAGE_TIME_UP")
 * @param container Pointer to pointer of the container object
 * @param in_cycle Whether this page should be included in cycle navigation (true) or skipped (false)
 * @return true if registration successful, false otherwise
 */
bool ui_bridge_register_page_with_cycle(const char *page_id, lv_obj_t **container, bool in_cycle);

/**
 * @brief Switch to specified page
 *
 * This function handles all page visibility and state management in one call.
 *
 * @param page_id Target page ID to switch to
 */
void ui_bridge_switch_page(const char *page_id);

/**
 * @brief Get current page ID
 *
 * @return Current page ID, or NULL if no page is active
 */
const char *ui_bridge_get_current_page(void);

/**
 * @brief Set page switch callback for custom handling
 *
 * When a gesture navigation occurs, this callback is called with the target page ID.
 * If callback returns true, the default page switching is skipped.
 *
 * @param cb Callback function (NULL to disable)
 * @param user_data User data to pass to callback
 */
void ui_bridge_set_page_switch_callback(ui_bridge_page_switch_cb_t cb, void *user_data);

bool ui_bridge_is_on_home_page(void);

#ifdef __cplusplus
}
#endif

#endif // LVGL_DISPLAY_BRIDGE_H
