/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "lvgl.h"

#define PAGE_SLEEP      "SLEEP"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create 24h sleep timer UI with parent container
 *
 * @param parent  Parent object to create the UI in
 * @return Created container object
 */
lv_obj_t *alarm_sleep_24h_create_with_parent(lv_obj_t *parent);

/**
 * @brief Trigger center button click event
 *
 * Simulates a click on the center button to toggle duration display
 */
void alarm_sleep_24h_trigger_center_btn(void);

/**
 * @brief Set end time only
 *
 * Sets only the end time for the sleep timer, precise to minutes.
 * The start time is automatically set to current time.
 *
 * @param end_hour End hour (0-23)
 * @param end_min  End minute (0-59)
 */
bool alarm_sleep_24h_set_end_time(int32_t end_hour, int32_t end_min);

bool alarm_sleep_24h_delete_end_time();
/**
 * @brief Get end time
 *
 * Gets the end time for the sleep timer.
 *
 * @param end_hour Pointer to store end hour (0-23), can be NULL
 * @param end_min  Pointer to store end minute (0-59), can be NULL
 * @return true if successful, false otherwise
 */
bool alarm_sleep_24h_get_end_time(int32_t *end_hour, int32_t *end_min);

/**
 * @brief Snooze sleep timer by adding minutes to current time
 *
 * Updates the end time to current time + specified minutes.
 *
 * @param minutes Number of minutes to add (default: 5)
 */
void alarm_sleep_24h_snooze(int32_t minutes);

/**
 * @brief Update sleep UI internal
 *
 * Updates the sleep UI internal.
 *
 * @param end_hour End hour (0-23)
 * @param end_min  End minute (0-59)
 */
void alarm_sleep_24h_update_ui_internal(int32_t end_hour, int32_t end_min);

#ifdef __cplusplus
}
#endif
