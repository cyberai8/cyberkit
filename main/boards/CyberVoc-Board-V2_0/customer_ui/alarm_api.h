#ifndef MAIN_UI_API_H
#define MAIN_UI_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file alarm_api.h.h
 * @brief Public API for main UI operations
 *
 * This file provides the public interface for external modules to interact
 * with the main UI system, including pomodoro timer control.
 */

/**
 * @brief Initialize main UI
 *
 * Creates and registers all UI containers (pomodoro, sleep, time up).
 * This function should be called after the display is initialized.
 */
void alarm_create_ui(void);

/**
 * @brief Show pomodoro timer page with specified minutes
 *
 * Configures the pomodoro timer to the specified duration and switches to the pomodoro page.
 *
 * @param minutes Timer duration in minutes (defaults to 5 if <= 0)
 */
void alarm_start_pomodoro(int32_t minutes);

/**
 * @brief Start pomodoro timer
 *
 * Starts the pomodoro timer if it's paused or stopped.
 */
bool alarm_resume_pomodoro(void);

/**
 * @brief Pause pomodoro timer
 *
 * Pauses the pomodoro timer if it's currently running.
 */
bool alarm_pause_pomodoro(void);

/**
 * @brief Show sleep timer page with specified end time
 *
 * Configures the sleep timer with current time as start time and specified end time,
 * and switches to the sleep page.
 *
 * @param end_hour End hour (0-23)
 * @param end_min  End minute (0-59)
 */
bool alarm_start_sleep(int32_t end_hour, int32_t end_min);
bool alarm_delete_sleep();
/**
 * @brief Set sleep end time only
 *
 * Sets only the end time for the sleep timer, precise to minutes.
 * The start time is automatically set to current time. Does not switch page.
 *
 * @param end_hour End hour (0-23)
 * @param end_min  End minute (0-59)
 */
void alarm_set_sleep_end_time(int32_t end_hour, int32_t end_min);

/**
 * @brief Get sleep end time
 *
 * Gets the stored end time for the sleep timer.
 *
 * @param end_hour Pointer to store end hour (0-23), can be NULL
 * @param end_min  Pointer to store end minute (0-59), can be NULL
 * @return true if successful, false otherwise
 */
bool alarm_get_sleep_end_time(int32_t *end_hour, int32_t *end_min);

#ifdef __cplusplus
}
#endif

#endif // MAIN_UI_API_H
