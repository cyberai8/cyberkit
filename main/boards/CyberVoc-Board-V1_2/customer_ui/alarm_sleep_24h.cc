/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <string.h>  /* For strcmp */
#include "alarm_manager.h"
#include "alarm_api.h"
#include "alarm_end.h"

#define DAY_SECONDS           (24 * 60 * 60)
#define DEFAULT_START_HOUR    10
#define DEFAULT_END_HOUR      11

static const char *TAG = "sleep_24h";

typedef struct {
    lv_obj_t *container;
    lv_obj_t *bg_arc;
    lv_obj_t *range_arc;
    lv_obj_t *time_container;
    lv_obj_t *start_hour_label;
    lv_obj_t *start_colon_label;
    lv_obj_t *start_min_label;
    lv_obj_t *start_ampm_label;
    lv_obj_t *center_sep_label;
    lv_obj_t *end_hour_label;
    lv_obj_t *end_colon_label;
    lv_obj_t *end_min_label;
    lv_obj_t *end_ampm_label;
    lv_obj_t *duration_label;
    lv_obj_t *center_btn;
    lv_obj_t *start_knob_img;
    lv_obj_t *end_knob_img;
    lv_timer_t *time_update_timer;
    int32_t start_angle;
    int32_t end_angle;
    bool show_duration;
    bool has_jumped_to_time_up;  /* Flag to prevent repeated jumps */
} alarm_sleep_24h_ui_t;

static alarm_sleep_24h_ui_t s_sleep_24h_ui;

/* Internal storage for sleep end time */
// static int32_t s_sleep_end_hour = 14;  /* Default end hour */
// static int32_t s_sleep_end_min = 20;   /* Default end minute */
// static bool s_sleep_end_enable = false; 

typedef struct 
{
     int32_t s_sleep_end_hour;
     int32_t s_sleep_end_min;
     bool s_sleep_end_enable;
     int32_t i;
} alarm_sleep_24h_clock;
static int32_t nummber = 10;
static int32_t now_num = 0;
static alarm_sleep_24h_clock alarm_sleep_clocks[10];


// static int32_t s_sleep_end_hour[10] = {14,14,14,14,14,14,14,14,14,14};  /* Default end hour */
// static int32_t s_sleep_end_min[10] = {30,30,30,30,30,30,30,30,30,30};   /* Default end minute */
// static bool s_sleep_end_enable[10] = {false,false,false,false,false,false,false,false,false,false}; 

static void get_current_time(int32_t *hour, int32_t *min)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    *hour = timeinfo.tm_hour;
    *min = timeinfo.tm_min;
}

static float calculate_angle_from_center(lv_coord_t center_x, lv_coord_t center_y,
                                         lv_coord_t point_x, lv_coord_t point_y)
{
    float dx = point_x - center_x;
    float dy = center_y - point_y;
    float angle_rad = atan2f(dy, dx);
    float angle_deg = angle_rad * 180.0f / (float)M_PI;

    angle_deg = 90.0f - angle_deg;

    if (angle_deg < 0) {
        angle_deg += 360.0f;
    } else if (angle_deg >= 360.0f) {
        angle_deg -= 360.0f;
    }

    return angle_deg;
}

static void update_knob_positions(alarm_sleep_24h_ui_t *ui)
{
    if (!ui->range_arc) {
        return;
    }

    lv_coord_t arc_size = lv_obj_get_width(ui->range_arc);
    lv_coord_t arc_width = 30;
    lv_coord_t radius = arc_size / 2 - arc_width / 2;
    lv_coord_t center_x = arc_size / 2;
    lv_coord_t center_y = arc_size / 2;

    lv_coord_t arc_x = lv_obj_get_x(ui->range_arc);
    lv_coord_t arc_y = lv_obj_get_y(ui->range_arc);

    /* Update start knob position (current time indicator) */
    if (ui->start_knob_img) {
        lv_coord_t start_w = lv_obj_get_width(ui->start_knob_img);
        lv_coord_t start_h = lv_obj_get_height(ui->start_knob_img);
        float start_rad = (90.0f - (float)ui->start_angle) * (float)M_PI / 180.0f;
        lv_coord_t start_x = arc_x + center_x + (lv_coord_t)(radius * cosf(start_rad)) - start_w / 2;
        lv_coord_t start_y = arc_y + center_y - (lv_coord_t)(radius * sinf(start_rad)) - start_h / 2;
        lv_obj_set_pos(ui->start_knob_img, start_x, start_y);
    }

    /* Update end knob position */
    if (ui->end_knob_img) {
        lv_coord_t end_w   = lv_obj_get_width(ui->end_knob_img);
        lv_coord_t end_h   = lv_obj_get_height(ui->end_knob_img);
        float end_rad   = (90.0f - (float)ui->end_angle)   * (float)M_PI / 180.0f;
        lv_coord_t end_x   = arc_x + center_x + (lv_coord_t)(radius * cosf(end_rad))   - end_w / 2;
        lv_coord_t end_y   = arc_y + center_y - (lv_coord_t)(radius * sinf(end_rad))   - end_h / 2;
        lv_obj_set_pos(ui->end_knob_img, end_x, end_y);
    }
}

static void update_start_time_display(alarm_sleep_24h_ui_t *ui)
{
    /* Get current time */
    int32_t start_hour_24, start_min;

    get_current_time(&start_hour_24, &start_min);

    /* Update start angle based on current time */
    int32_t start_total_sec = start_hour_24 * 3600 + start_min * 60;
    ui->start_angle = (start_total_sec * 360) / DAY_SECONDS;

    /* Format for display */
    int32_t start_hour_12 = start_hour_24 % 12;
    if (start_hour_12 == 0) {
        start_hour_12 = 12;
    }
    const char *start_ampm = (start_hour_24 < 12) ? "AM" : "PM";

    /* Update labels */
    if (ui->start_hour_label && ui->start_min_label && ui->start_ampm_label) {
        lv_label_set_text_fmt(ui->start_hour_label, "%ld", (long)start_hour_12);
        lv_label_set_text_fmt(ui->start_min_label, "%02ld", (long)start_min);
        lv_label_set_text(ui->start_ampm_label, start_ampm);
    }
}

static void update_range_from_angles(alarm_sleep_24h_ui_t *ui, bool sync_from_storage)
{
    /* Start time is always current time */
    update_start_time_display(ui);

    int32_t start_hour_24, start_min;
    get_current_time(&start_hour_24, &start_min);

    int32_t end_hour, end_min;
    int32_t end_total_sec;

    if (sync_from_storage) {
        /* Get stored end time from internal storage and sync to angle */


        //选择最近的闹钟显示
        for(int i=0;i<now_num;i++)
        {
            if(alarm_sleep_clocks[i].s_sleep_end_hour >= start_hour_24 && alarm_sleep_clocks[i].s_sleep_end_min >= start_min)
            {
                end_hour = alarm_sleep_clocks[i].s_sleep_end_hour;
                end_min = alarm_sleep_clocks[i].s_sleep_end_min;

                break;
            }
            else
            {
                end_hour = 0;
                end_min = 0;
            }
        }
        ESP_LOGI(TAG,"当前闹钟： %02ld:%02ld", (long)end_hour, (long)end_min);
        end_total_sec = end_hour * 3600 + end_min * 60;
        ui->end_angle = (end_total_sec * 360) / DAY_SECONDS;
    } else {
        /* Calculate end time from current angle and update global storage */
        end_total_sec = (ui->end_angle * DAY_SECONDS) / 360;
        end_hour = end_total_sec / 3600;
        end_min = (end_total_sec % 3600) / 60;
        
        /* Validate and update internal storage */
        if (end_hour < 0 || end_hour >= 24) {
            end_hour = DEFAULT_END_HOUR;
        }
        if (end_min < 0 || end_min >= 60) {
            end_min = 0;
        }
        
        /* Update internal storage */
        // s_sleep_end_hour = end_hour;
        // s_sleep_end_min = end_min;
        
        ESP_LOGD(TAG, "End time updated from drag: %02ld:%02ld", (long)end_hour, (long)end_min);
    }

    int32_t start_total_sec = (ui->start_angle * DAY_SECONDS) / 360;

    int32_t duration_sec;
    if (end_total_sec >= start_total_sec) {
        duration_sec = end_total_sec - start_total_sec;
    } else {
        duration_sec = (DAY_SECONDS - start_total_sec) + end_total_sec;
    }
    float duration_hours = duration_sec / 3600.0f;

    int32_t end_hour_12 = end_hour % 12;
    if (end_hour_12 == 0) {
        end_hour_12 = 12;
    }
    const char *end_ampm = (end_hour < 12) ? "AM" : "PM";

    if (ui->show_duration) {
        if (ui->time_container) {
            lv_obj_add_flag(ui->time_container, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui->duration_label) {
            lv_obj_clear_flag(ui->duration_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(ui->duration_label, "%.1f hr", duration_hours);
        }
    } else {
        if (ui->duration_label) {
            lv_obj_add_flag(ui->duration_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui->time_container) {
            lv_obj_clear_flag(ui->time_container, LV_OBJ_FLAG_HIDDEN);
        }

        /* Start time is already updated by update_start_time_display() */
        /* Only update end time display */
        if (ui->center_sep_label && ui->end_hour_label && ui->end_min_label && ui->end_ampm_label) {
            lv_label_set_text(ui->center_sep_label, "|");

            lv_label_set_text_fmt(ui->end_hour_label, "%ld", (long)end_hour_12);
            lv_label_set_text_fmt(ui->end_min_label, "%02ld", (long)end_min);
            lv_label_set_text(ui->end_ampm_label, end_ampm);
        }
    }

    lv_arc_set_angles(ui->range_arc, (int16_t)ui->start_angle, (int16_t)ui->end_angle);
    update_knob_positions(ui);
}

static void get_arc_center_abs(lv_obj_t *arc, lv_coord_t *center_x, lv_coord_t *center_y)
{
    lv_coord_t x = 0;
    lv_coord_t y = 0;
    lv_obj_t *obj = arc;

    while (obj != NULL) {
        x += lv_obj_get_x(obj);
        y += lv_obj_get_y(obj);
        obj = lv_obj_get_parent(obj);
        if (obj == lv_scr_act()) {
            break;
        }
    }

    x += lv_obj_get_width(arc) / 2;
    y += lv_obj_get_height(arc) / 2;

    *center_x = x;
    *center_y = y;
}

static void time_update_timer_cb(lv_timer_t *timer)
{
    alarm_sleep_24h_ui_t *ui = (alarm_sleep_24h_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->container) {
        return;
    }

    if(now_num==0)
    {
        return;
    }
    // if(!s_sleep_end_enable)
    // {
    //     return;
    // }
    /* Check if end time equals current time (check regardless of page visibility) */
    int32_t current_hour, current_min;

    get_current_time(&current_hour, &current_min);
    int32_t current_total_sec;
    current_total_sec = current_hour * 3600 + current_min * 60;
    if(current_total_sec == 0)
    {
        for(int i=0;i<now_num;i++)
        {
            alarm_sleep_clocks[i].s_sleep_end_enable = true;
        }
    }
    
    for(int i=0;i<now_num;i++)
    {
        get_current_time(&current_hour, &current_min);
        current_total_sec = current_hour * 3600 + current_min * 60;
        if(alarm_sleep_clocks[i].s_sleep_end_enable)
        {

            int32_t end_hour = alarm_sleep_clocks[i].s_sleep_end_hour;
            int32_t end_min = alarm_sleep_clocks[i].s_sleep_end_min;
            int32_t end_total_sec = end_hour * 3600 + end_min * 60;
            ESP_LOGI(TAG, "当前时分对应: end-now:%02ld:%02ld--%02ld:%02ld", (long)end_hour, (long)end_min,(long)current_hour, (long)current_min);
            //ESP_LOGI(TAG, "当前时分对应: end-now:%02ld--%02ld", (long)end_total_sec, (long)current_total_sec);
            ESP_LOGI(TAG, "has_jumped_to_time_up: %d", ui->has_jumped_to_time_up);
            if (end_total_sec == current_total_sec && !ui->has_jumped_to_time_up) {
                ESP_LOGI(TAG, "Time up, end time: %02ld:%02ld", (long)end_hour, (long)end_min);
                //alarm_sleep_clocks[i].s_sleep_end_enable = false;
                ui->has_jumped_to_time_up = true;
                alarm_time_up_set_origin(PAGE_SLEEP);
                main_ui_switch_page(PAGE_TIME_UP);
                return;
            } else if (end_total_sec != current_total_sec) {
                //alarm_sleep_clocks[i].s_sleep_end_enable = true;
                ui->has_jumped_to_time_up = false;
            }

            /* Log every minute when minute changes */
            static int32_t last_logged_min = -1;
            if (current_min != last_logged_min) {
                last_logged_min = current_min;
                ESP_LOGI(TAG, "range: start=%ld° -> %02ld:%02ld, end=%ld° -> %02ld:%02ld",
                        (long)ui->start_angle, (long)current_hour, (long)current_min,
                        (long)ui->end_angle, (long)end_hour, (long)end_min);
            }

            /* Check if container is visible */
            if (lv_obj_has_flag(ui->container, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }

            update_start_time_display(ui);
            update_range_from_angles(ui, true);  /* Sync from storage */
        }

    }

    
}

static void center_btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    alarm_sleep_24h_ui_t *ui = (alarm_sleep_24h_ui_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        ui->show_duration = !ui->show_duration;
        ESP_LOGI(TAG, "Toggled duration display: %s", ui->show_duration ? "ON" : "OFF");
        update_range_from_angles(ui, true);  /* Don't sync, use current angle */
    }
}

static void end_knob_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    alarm_sleep_24h_ui_t *ui = (alarm_sleep_24h_ui_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_t *knob = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_set_style_transform_scale(knob, 280, 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_t *knob = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_set_style_transform_scale(knob, 256, 0);
    }

    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) {
            return;
        }

        lv_point_t point;
        lv_indev_get_point(indev, &point);

        lv_coord_t cx = 0;
        lv_coord_t cy = 0;
        get_arc_center_abs(ui->range_arc, &cx, &cy);

        float angle = calculate_angle_from_center(cx, cy, point.x, point.y);
        if (angle < 0.0f) {
            angle = 0.0f;
        } else if (angle > 360.0f) {
            angle = 360.0f;
        }

        ui->end_angle = (int32_t)angle;
        /* Update from angle (don't sync from storage, update storage from angle) */
        update_range_from_angles(ui, false);
    }
}

lv_obj_t *alarm_sleep_24h_create_with_parent(lv_obj_t *parent)
{
    s_sleep_24h_ui.start_angle = (DEFAULT_START_HOUR * 360) / 24;
    s_sleep_24h_ui.end_angle   = (DEFAULT_END_HOUR   * 360) / 24;
    s_sleep_24h_ui.has_jumped_to_time_up = false;

    s_sleep_24h_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_sleep_24h_ui.container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_sleep_24h_ui.container, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_sleep_24h_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_sleep_24h_ui.container, 0, 0);
    lv_obj_clear_flag(s_sleep_24h_ui.container, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t arc_size = LV_MIN(SCREEN_WIDTH, SCREEN_HEIGHT) - 20;

    s_sleep_24h_ui.bg_arc = lv_arc_create(s_sleep_24h_ui.container);
    lv_obj_set_size(s_sleep_24h_ui.bg_arc, arc_size + 20, arc_size + 20);
    lv_obj_center(s_sleep_24h_ui.bg_arc);
    lv_arc_set_rotation(s_sleep_24h_ui.bg_arc, 270);
    lv_arc_set_bg_angles(s_sleep_24h_ui.bg_arc, 0, 360);
    lv_obj_remove_style(s_sleep_24h_ui.bg_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_sleep_24h_ui.bg_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sleep_24h_ui.bg_arc, 50, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sleep_24h_ui.bg_arc, lv_color_make(0x56, 0x56, 0x56), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sleep_24h_ui.bg_arc, 0, LV_PART_INDICATOR);
    lv_arc_set_value(s_sleep_24h_ui.bg_arc, 0);

    s_sleep_24h_ui.range_arc = lv_arc_create(s_sleep_24h_ui.container);
    lv_obj_set_size(s_sleep_24h_ui.range_arc, arc_size, arc_size);
    lv_obj_center(s_sleep_24h_ui.range_arc);
    lv_arc_set_rotation(s_sleep_24h_ui.range_arc, 270);
    lv_arc_set_mode(s_sleep_24h_ui.range_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(s_sleep_24h_ui.range_arc, 0, 360);
    lv_arc_set_range(s_sleep_24h_ui.range_arc, 0, 360);

    lv_arc_set_angles(s_sleep_24h_ui.range_arc,
                      (int16_t)s_sleep_24h_ui.start_angle,
                      (int16_t)s_sleep_24h_ui.end_angle);

    lv_obj_set_style_arc_image_src(s_sleep_24h_ui.range_arc, &time_arc_texture, LV_PART_INDICATOR);
    lv_obj_remove_style(s_sleep_24h_ui.range_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_sleep_24h_ui.range_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sleep_24h_ui.range_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sleep_24h_ui.range_arc, 30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_sleep_24h_ui.range_arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sleep_24h_ui.range_arc, lv_color_make(0x56, 0x56, 0x56), LV_PART_INDICATOR);

    lv_obj_t *watch_bg_img = lv_img_create(s_sleep_24h_ui.container);
    lv_img_set_src(watch_bg_img, &watch_bg);
    lv_obj_center(watch_bg_img);
    lv_obj_clear_flag(watch_bg_img, LV_OBJ_FLAG_SCROLLABLE);

    s_sleep_24h_ui.time_container = lv_obj_create(s_sleep_24h_ui.container);
    lv_obj_set_size(s_sleep_24h_ui.time_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_sleep_24h_ui.time_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sleep_24h_ui.time_container, 0, 0);
    lv_obj_set_style_pad_all(s_sleep_24h_ui.time_container, 0, 0);
    lv_obj_center(s_sleep_24h_ui.time_container);

    static const int32_t col_dsc[] = {
        20,
        4,
        30,
        35,
        LV_GRID_TEMPLATE_LAST
    };
    static const int32_t row_dsc[] = {
        LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_grid_dsc_array(s_sleep_24h_ui.time_container, col_dsc, row_dsc);

    s_sleep_24h_ui.start_hour_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.start_hour_label, "10");
    lv_obj_set_size(s_sleep_24h_ui.start_hour_label, 20, LV_SIZE_CONTENT);
    lv_obj_set_grid_cell(s_sleep_24h_ui.start_hour_label,
                         LV_GRID_ALIGN_END, 0, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    s_sleep_24h_ui.start_colon_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.start_colon_label, ":");
    lv_obj_set_grid_cell(s_sleep_24h_ui.start_colon_label,
                         LV_GRID_ALIGN_CENTER, 1, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_style_text_font(s_sleep_24h_ui.start_colon_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.start_colon_label, lv_color_white(), 0);

    s_sleep_24h_ui.start_min_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.start_min_label, "00");
    lv_obj_set_size(s_sleep_24h_ui.start_min_label, 30, LV_SIZE_CONTENT);
    lv_obj_set_grid_cell(s_sleep_24h_ui.start_min_label,
                         LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    s_sleep_24h_ui.start_ampm_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.start_ampm_label, "PM");
    lv_obj_set_size(s_sleep_24h_ui.start_ampm_label, 35, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_sleep_24h_ui.start_ampm_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(s_sleep_24h_ui.start_ampm_label,
                         LV_GRID_ALIGN_START, 3, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    s_sleep_24h_ui.center_sep_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.center_sep_label, "|");
    lv_obj_set_grid_cell(s_sleep_24h_ui.center_sep_label,
                         LV_GRID_ALIGN_CENTER, 1, 1,
                         LV_GRID_ALIGN_CENTER, 1, 1);

    s_sleep_24h_ui.end_hour_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.end_hour_label, "7");
    lv_obj_set_size(s_sleep_24h_ui.end_hour_label, 20, LV_SIZE_CONTENT);
    lv_obj_set_grid_cell(s_sleep_24h_ui.end_hour_label,
                         LV_GRID_ALIGN_END, 0, 1,
                         LV_GRID_ALIGN_CENTER, 2, 1);

    s_sleep_24h_ui.end_colon_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.end_colon_label, ":");
    lv_obj_set_grid_cell(s_sleep_24h_ui.end_colon_label,
                         LV_GRID_ALIGN_CENTER, 1, 1,
                         LV_GRID_ALIGN_CENTER, 2, 1);
    lv_obj_set_style_text_font(s_sleep_24h_ui.end_colon_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.end_colon_label, lv_color_white(), 0);

    s_sleep_24h_ui.end_min_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.end_min_label, "30");
    lv_obj_set_size(s_sleep_24h_ui.end_min_label, 30, LV_SIZE_CONTENT);
    lv_obj_set_grid_cell(s_sleep_24h_ui.end_min_label,
                         LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_CENTER, 2, 1);

    s_sleep_24h_ui.end_ampm_label = lv_label_create(s_sleep_24h_ui.time_container);
    lv_label_set_text(s_sleep_24h_ui.end_ampm_label, "AM");
    lv_obj_set_size(s_sleep_24h_ui.end_ampm_label, 35, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_sleep_24h_ui.end_ampm_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(s_sleep_24h_ui.end_ampm_label,
                         LV_GRID_ALIGN_START, 3, 1,
                         LV_GRID_ALIGN_CENTER, 2, 1);

    lv_obj_set_style_text_font(s_sleep_24h_ui.start_hour_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.start_min_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.start_ampm_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.center_sep_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.end_hour_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.end_min_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(s_sleep_24h_ui.end_ampm_label, &lv_font_montserrat_20, 0);

    lv_obj_set_style_text_color(s_sleep_24h_ui.start_hour_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.start_min_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.start_ampm_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.center_sep_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.end_hour_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.end_min_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.end_ampm_label, lv_color_white(), 0);

    lv_obj_set_style_text_align(s_sleep_24h_ui.start_ampm_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(s_sleep_24h_ui.end_ampm_label, LV_TEXT_ALIGN_LEFT, 0);

    s_sleep_24h_ui.duration_label = lv_label_create(s_sleep_24h_ui.container);
    lv_label_set_text(s_sleep_24h_ui.duration_label, "9.5 hr");
    lv_obj_set_style_text_font(s_sleep_24h_ui.duration_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_sleep_24h_ui.duration_label, lv_color_white(), 0);
    lv_obj_center(s_sleep_24h_ui.duration_label);
    lv_obj_add_flag(s_sleep_24h_ui.duration_label, LV_OBJ_FLAG_HIDDEN);

    s_sleep_24h_ui.center_btn = lv_btn_create(s_sleep_24h_ui.container);
    lv_obj_set_size(s_sleep_24h_ui.center_btn, 120, 120);
    lv_obj_center(s_sleep_24h_ui.center_btn);
    lv_obj_set_style_bg_opa(s_sleep_24h_ui.center_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_sleep_24h_ui.center_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(s_sleep_24h_ui.center_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_sleep_24h_ui.center_btn, center_btn_event_handler, LV_EVENT_CLICKED, &s_sleep_24h_ui);

    s_sleep_24h_ui.show_duration = false;

    /* Create start knob image (display only, not clickable - shows current time position) */
    s_sleep_24h_ui.start_knob_img = lv_img_create(s_sleep_24h_ui.container);
    lv_img_set_src(s_sleep_24h_ui.start_knob_img, &time_start);
    lv_obj_clear_flag(s_sleep_24h_ui.start_knob_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sleep_24h_ui.start_knob_img, LV_OBJ_FLAG_CLICKABLE);  /* Not clickable - read-only indicator */

    s_sleep_24h_ui.end_knob_img = lv_img_create(s_sleep_24h_ui.container);
    lv_img_set_src(s_sleep_24h_ui.end_knob_img, &time_sleep);
    lv_obj_clear_flag(s_sleep_24h_ui.end_knob_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sleep_24h_ui.end_knob_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sleep_24h_ui.end_knob_img, end_knob_event_handler, LV_EVENT_ALL, &s_sleep_24h_ui);

    lv_obj_update_layout(s_sleep_24h_ui.container);
    update_range_from_angles(&s_sleep_24h_ui, true);  /* Sync from storage */

    /* Create timer to update start time (current time) every minute */
    s_sleep_24h_ui.time_update_timer = lv_timer_create(time_update_timer_cb, 5000, &s_sleep_24h_ui);  /* Update every minute */
    lv_timer_set_repeat_count(s_sleep_24h_ui.time_update_timer, -1);  /* Repeat indefinitely */

    ESP_LOGI(TAG, "Sleep 24h UI created: start=%ld° (current time), end=%ld°",
             (long)s_sleep_24h_ui.start_angle, (long)s_sleep_24h_ui.end_angle);

    lv_obj_add_flag(s_sleep_24h_ui.container, LV_OBJ_FLAG_HIDDEN);

    return s_sleep_24h_ui.container;
}

void alarm_sleep_24h_trigger_center_btn(void)
{
    if (s_sleep_24h_ui.center_btn) {
        esp_lv_adapter_lock(-1);
        lv_obj_send_event(s_sleep_24h_ui.center_btn, LV_EVENT_CLICKED, NULL);
        esp_lv_adapter_unlock();
    }
}

/* Internal function to update UI only (no global storage) */
void alarm_sleep_24h_update_ui_internal(int32_t end_hour, int32_t end_min)
{
    /* Get current time as start time */
    int32_t start_hour, start_min;
    get_current_time(&start_hour, &start_min);

    /* Calculate total seconds from midnight */
    int32_t start_total_sec = start_hour * 3600 + start_min * 60;
    int32_t end_total_sec = end_hour * 3600 + end_min * 60;

    /* Convert to angles (0-360 degrees) */
    s_sleep_24h_ui.start_angle = (start_total_sec * 360) / DAY_SECONDS;
    s_sleep_24h_ui.end_angle = (end_total_sec * 360) / DAY_SECONDS;
    s_sleep_24h_ui.has_jumped_to_time_up = false;  /* Reset flag when time is set */

    ESP_LOGI(TAG, "Update sleep UI: %02ld:%02ld (current) -> %02ld:%02ld (start=%ld°, end=%ld°)",
             (long)start_hour, (long)start_min, (long)end_hour, (long)end_min,
             (long)s_sleep_24h_ui.start_angle, (long)s_sleep_24h_ui.end_angle);

    /* Update UI if container exists */
    if (s_sleep_24h_ui.container) {
        esp_lv_adapter_lock(-1);
        update_range_from_angles(&s_sleep_24h_ui, true);  /* Sync from storage */
        esp_lv_adapter_unlock();
    }
}

bool alarm_sleep_24h_set_end_time(int32_t end_hour, int32_t end_min)
{
    if(now_num >= nummber)
    {
        ESP_LOGW(TAG, "闹钟数量超过10个", );
        return false;
    }
    /* Validate end time parameters */
    if (end_hour < 0 || end_hour >= 24) {
        ESP_LOGW(TAG, "Invalid end_hour: %ld, using default", (long)end_hour);
        end_hour = DEFAULT_END_HOUR;
    }
    if (end_min < 0 || end_min >= 60) {
        ESP_LOGW(TAG, "Invalid end_min: %ld, using 0", (long)end_min);
        end_min = 0;
    }
    //if(now_num==0) now_num=1;
    for(int i=0;i<=now_num;i++)
    {
        if(!alarm_sleep_clocks[i].s_sleep_end_enable)
        {
            alarm_sleep_clocks[i].s_sleep_end_hour = end_hour;
            alarm_sleep_clocks[i].s_sleep_end_min = end_min;
            alarm_sleep_clocks[i].s_sleep_end_enable = true;
            ESP_LOGI(TAG, "添加第%d个闹钟: %02ld:%02ld", i+1,(long)alarm_sleep_clocks[i].s_sleep_end_hour, (long)alarm_sleep_clocks[i].s_sleep_end_min);
            ESP_LOGI(TAG, "当前有: %02ld个闹钟", (long)++now_num);
            break;
        }
    }
    //now_num++;
    /* Store end time internally */


    ESP_LOGI(TAG, "Set sleep end time: %02ld:%02ld", (long)end_hour, (long)end_min);

    /* Update UI */
    alarm_sleep_24h_update_ui_internal(end_hour, end_min);
    return true;
}

bool alarm_sleep_24h_delete_end_time()
{
    //if(now_num==0) now_num=1;
    ESP_LOGI(TAG, "删除所有闹钟");
    for(int i=0;i<now_num;i++)
    {
        if(alarm_sleep_clocks[i].s_sleep_end_enable)
        {
            alarm_sleep_clocks[i].s_sleep_end_enable = false;
            
        }
    }
    /* Store end time internally */
    now_num = 0;


    /* Update UI */
    return true;
}

bool alarm_sleep_24h_get_end_time(int32_t *end_hour, int32_t *end_min)
{
    if (end_hour == NULL || end_min == NULL) {
        return false;
    }
    for(int i=0;i<now_num;i++)
    {
        if(alarm_sleep_clocks[i].s_sleep_end_enable)
        {
            *end_hour = alarm_sleep_clocks[i].s_sleep_end_hour;
            *end_min = alarm_sleep_clocks[i].s_sleep_end_min;
            alarm_sleep_clocks[i].s_sleep_end_enable = false;
            break;
        }        
    }

           


    return true;
}

void alarm_sleep_24h_snooze(int32_t minutes)
{
    if (minutes <= 0) {
        minutes = 5;  /* Default to 5 minutes */
    }

    /* Get current time */
    int32_t current_hour, current_min;
    get_current_time(&current_hour, &current_min);

    /* Calculate current time + minutes */
    int32_t current_min_total = current_hour * 60 + current_min;
    int32_t new_min_total = current_min_total + minutes;
    int32_t new_hour = new_min_total / 60;
    int32_t new_min = new_min_total % 60;

    /* Handle day overflow (24-hour format) */
    new_hour = new_hour % 24;

    ESP_LOGI(TAG, "Snooze timer: %02ld:%02ld -> %02ld:%02ld (+%ld min)",
             (long)current_hour, (long)current_min, (long)new_hour, (long)new_min, (long)minutes);

    /* Update end time */
    alarm_sleep_24h_set_end_time(new_hour, new_min);
}
