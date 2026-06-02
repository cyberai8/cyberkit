/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#define PAGE_POMODORO   "POMODORO"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create pomodoro timer UI with parent container
 *
 * @param parent  Parent object to create the UI in
 * @return Created container object
 */
lv_obj_t *alarm_pomodoro_create_with_parent(lv_obj_t *parent);

/**
 * @brief Adjust end point by delta minutes
 *
 * @param delta_minutes  Positive value to increase time, negative to decrease (unit: minutes)
 */
void alarm_pomodoro_adjust_end_point(int32_t delta_minutes);

/**
 * @brief Toggle start/pause timer
 *
 * If timer is running, pause it. If paused, resume it (if there's remaining time).
 */
void alarm_pomodoro_toggle_start_pause(void);

/**
 * @brief Reset timer to 0:00
 */
void alarm_pomodoro_reset_to_zero(void);

/* Internal functions - declared here for internal use only */
/* External modules should use customer_ui_api.h instead */
bool alarm_pomodoro_is_paused(void);
void alarm_pomodoro_start(void);
void alarm_pomodoro_pause(void);

#ifdef __cplusplus
}
#endif
