#include <string.h>  /* For strcmp */
#include <esp_log.h>
#include <esp_lv_adapter.h>
//#include <mmap_generate_storage.h>
#include "alarm_manager.h"
#include "alarm_api.h"

#define TAG "alarm_manager"

// ============================================================================
// Type Definitions
// ============================================================================

// ============================================================================
// Static Variables
// ============================================================================

/* UI container objects (main_ui specific) */
static lv_obj_t *container_pomodoro = NULL;
static lv_obj_t *container_sleep = NULL;
static lv_obj_t *container_time_up = NULL;


// ============================================================================
// Global Variables
// ============================================================================

void main_ui_switch_page(const char *page_name)
{
    ESP_LOGD(TAG, "Switching to page: %s", page_name);
    ui_bridge_switch_page(page_name);
}

static bool main_ui_page_switch_callback(const char *target_page, void *user_data)
{
    const char *current_page = ui_bridge_get_current_page();
    ESP_LOGI(TAG, "Page switch: %s -> %s", current_page ? current_page : "NULL", target_page ? target_page : "NULL");

    /* Special handling for pomodoro page */
    if (target_page != NULL && strcmp(target_page, PAGE_POMODORO) == 0 &&
            (current_page == NULL || strcmp(current_page, PAGE_POMODORO) != 0)) {
        alarm_start_pomodoro(5);
        return true;  /* Handled, skip default switch */
    }

    return false;  /* Use default switch */
}

void alarm_create_ui()
{
    lv_obj_t *scr = lv_scr_act();
    /* Create and register pomodoro container */
    container_pomodoro = alarm_pomodoro_create_with_parent(scr);
    ui_bridge_register_page_with_cycle(PAGE_POMODORO, &container_pomodoro, true);

    /* Create and register sleep container */
    container_sleep = alarm_sleep_24h_create_with_parent(scr);
    ui_bridge_register_page_with_cycle(PAGE_SLEEP, &container_sleep, true);

    /* Create and register time up container */
    container_time_up = alarm_time_up_create_with_parent(scr);
    ui_bridge_register_page_with_cycle(PAGE_TIME_UP, &container_time_up, false);

    /* Register page switch callback for custom handling (e.g., pomodoro) */
    ui_bridge_set_page_switch_callback(main_ui_page_switch_callback, NULL);
}

void alarm_start_pomodoro(int32_t minutes)
{
    if (minutes <= 0) {
        minutes = 5;
    }

    ESP_LOGI(TAG, "Show pomodoro page with %ld minutes", (long)minutes);

    /* Configure pomodoro timer first, then switch page */
    alarm_pomodoro_reset_to_zero();
    alarm_pomodoro_adjust_end_point(minutes);
    main_ui_switch_page(PAGE_POMODORO);
}

bool alarm_resume_pomodoro(void)
{
    ESP_LOGI(TAG, "Start pomodoro timer");
    const char *current_page = ui_bridge_get_current_page();
    if (current_page == NULL || strcmp(current_page, PAGE_POMODORO) != 0) {
        ESP_LOGI(TAG, "Not on pomodoro page (current=%s)",
                 current_page ? current_page : "NULL");
        return false;
    } else {
        alarm_pomodoro_start();
    }
    return true;
}

bool alarm_pause_pomodoro(void)
{
    ESP_LOGI(TAG, "Pause pomodoro timer");
    const char *current_page = ui_bridge_get_current_page();
    if (current_page == NULL || strcmp(current_page, PAGE_POMODORO) != 0) {
        ESP_LOGI(TAG, "Not on pomodoro page (current=%s)",
                 current_page ? current_page : "NULL");
        return false;
    } else {
        alarm_pomodoro_pause();
    }
    return true;
}

bool alarm_start_sleep(int32_t end_hour, int32_t end_min)
{
    /* Validate and clamp values */
    const int32_t DEFAULT_END_HOUR = 8;

    if (end_hour < 0 || end_hour >= 24) {
        ESP_LOGW(TAG, "Invalid end_hour: %ld, using default %ld", (long)end_hour, (long)DEFAULT_END_HOUR);
        end_hour = DEFAULT_END_HOUR;
    }
    if (end_min < 0 || end_min >= 60) {
        ESP_LOGW(TAG, "Invalid end_min: %ld, using 0", (long)end_min);
        end_min = 0;
    }

    ESP_LOGI(TAG, "Show sleep page with end time: %02ld:%02ld (start time will be current time)",
             (long)end_hour, (long)end_min);

    /* Configure sleep timer first (start time is current time), then switch page */
    if(alarm_sleep_24h_set_end_time(end_hour, end_min))
    {
        main_ui_switch_page(PAGE_SLEEP);
        
    }
    else
    {
        return false;        
    } 

    return true;
}
bool alarm_delete_sleep()
{
    return alarm_sleep_24h_delete_end_time();
}
void alarm_set_sleep_end_time(int32_t end_hour, int32_t end_min)
{
    /* Validate and clamp values */
    const int32_t DEFAULT_END_HOUR = 8;

    if (end_hour < 0 || end_hour >= 24) {
        ESP_LOGW(TAG, "Invalid end_hour: %ld, using default %ld", (long)end_hour, (long)DEFAULT_END_HOUR);
        end_hour = DEFAULT_END_HOUR;
    }
    if (end_min < 0 || end_min >= 60) {
        ESP_LOGW(TAG, "Invalid end_min: %ld, using 0", (long)end_min);
        end_min = 0;
    }

    /* Delegate to sleep_24h module */
    alarm_sleep_24h_set_end_time(end_hour, end_min);
}

bool alarm_get_sleep_end_time(int32_t *end_hour, int32_t *end_min)
{
    /* Delegate to sleep_24h module */
    return alarm_sleep_24h_get_end_time(end_hour, end_min);
}
