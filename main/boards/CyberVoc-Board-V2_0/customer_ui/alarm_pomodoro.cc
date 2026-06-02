/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include "lv_eaf.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>
#include <string.h>  /* For strcmp */
#include "alarm_manager.h"

#define TIMER_MAX_SECONDS       (60 * 60)
#define TIMER_INITIAL_SECONDS   (0 * 60)
#define TIMER_UPDATE_PERIOD_MS  1000

static const char *TAG = "pomodoro";

typedef enum {
    TIMER_STATE_RUNNING = 0,
    TIMER_STATE_END,
    TIMER_STATE_PAUSED,
} pomodoro_state_t;

typedef struct {
    lv_obj_t *container;
    lv_obj_t *bg_arc;
    lv_obj_t *progress_arc;
    lv_obj_t *time_container;
    lv_obj_t *time_min_label;
    lv_obj_t *time_colon_label;
    lv_obj_t *time_sec_label;
    lv_obj_t *start_knob_img;
    lv_obj_t *end_knob_img;
    lv_obj_t *toggle_btn;  /* Transparent button for start/pause toggle */
    lv_obj_t *eaf_anim;
    lv_timer_t *timer;
    int32_t remaining_seconds;
    pomodoro_state_t state;
    bool was_running_before_zero;
} alarm_pomodoro_ui_t;

static alarm_pomodoro_ui_t s_pomodoro_ui;
static lv_image_dsc_t s_eaf_dsc;

static void pomodoro_set_state(alarm_pomodoro_ui_t *ui, pomodoro_state_t new_state)
{
    if (ui->state == new_state) {
        return;
    }

    ui->state = new_state;

    if (ui->eaf_anim) {
        if (new_state == TIMER_STATE_END || new_state == TIMER_STATE_PAUSED) {
            lv_eaf_pause(ui->eaf_anim);
        } else {
            lv_eaf_resume(ui->eaf_anim);
        }
    }
}

static float calculate_angle_from_center(lv_coord_t center_x, lv_coord_t center_y, lv_coord_t point_x, lv_coord_t point_y)
{
    float dx = point_x - center_x;
    float dy = center_y - point_y;
    float angle_rad = atan2(dy, dx);
    float angle_deg = angle_rad * 180.0f / M_PI;

    angle_deg = 90.0f - angle_deg;

    if (angle_deg < 0) {
        angle_deg += 360.0f;
    }

    return angle_deg;
}

static void update_knob_positions(alarm_pomodoro_ui_t *ui)
{
    if (!ui->progress_arc || !ui->start_knob_img || !ui->end_knob_img) {
        return;
    }

    lv_coord_t arc_size = lv_obj_get_width(ui->progress_arc);
    lv_coord_t arc_width = 30;
    lv_coord_t radius = arc_size / 2 - arc_width / 2;
    lv_coord_t center_x = arc_size / 2;
    lv_coord_t center_y = arc_size / 2;

    lv_coord_t arc_x = lv_obj_get_x(ui->progress_arc);
    lv_coord_t arc_y = lv_obj_get_y(ui->progress_arc);

    lv_coord_t start_w = lv_obj_get_width(ui->start_knob_img);
    lv_coord_t start_h = lv_obj_get_height(ui->start_knob_img);
    lv_coord_t end_w   = lv_obj_get_width(ui->end_knob_img);
    lv_coord_t end_h   = lv_obj_get_height(ui->end_knob_img);

    float start_angle_lvgl = 0.0f;
    float start_rad = (90.0f - start_angle_lvgl) * M_PI / 180.0f;
    lv_coord_t start_x = arc_x + center_x + (lv_coord_t)(radius * cos(start_rad)) - start_w / 2;
    lv_coord_t start_y = arc_y + center_y - (lv_coord_t)(radius * sin(start_rad)) - start_h / 2;

    int16_t end_angle_lvgl = (360 * ui->remaining_seconds) / TIMER_MAX_SECONDS;
    float end_rad = (90.0f - end_angle_lvgl) * M_PI / 180.0f;
    lv_coord_t end_x = arc_x + center_x + (lv_coord_t)(radius * cos(end_rad)) - end_w / 2;
    lv_coord_t end_y = arc_y + center_y - (lv_coord_t)(radius * sin(end_rad)) - end_h / 2;

    lv_obj_set_pos(ui->start_knob_img, start_x, start_y);
    lv_obj_set_pos(ui->end_knob_img, end_x, end_y);

}

static void update_time_from_angle(alarm_pomodoro_ui_t *ui, int32_t angle_deg)
{
    if (angle_deg < 0) {
        angle_deg = 0;
    }
    if (angle_deg > 360) {
        angle_deg = 360;
    }

    ui->remaining_seconds = (angle_deg * TIMER_MAX_SECONDS) / 360;

    if (ui->remaining_seconds <= 0) {
        ui->remaining_seconds = 0;
        pomodoro_set_state(ui, TIMER_STATE_END);
        ui->was_running_before_zero = false;
    } else {
        pomodoro_set_state(ui, TIMER_STATE_RUNNING);
    }

    int minutes = ui->remaining_seconds / 60;
    int seconds = ui->remaining_seconds % 60;
    if (ui->time_min_label && ui->time_sec_label) {
        lv_label_set_text_fmt(ui->time_min_label, "%02d", minutes);
        lv_label_set_text_fmt(ui->time_sec_label, "%02d", seconds);
    }

    lv_arc_set_angles(ui->progress_arc, 0, angle_deg);
    update_knob_positions(ui);
}

static void end_knob_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    alarm_pomodoro_ui_t *ui = (alarm_pomodoro_ui_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        if (code == LV_EVENT_PRESSED) {
            if (ui->timer) {
                lv_timer_pause(ui->timer);
            }
        }

        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) {
            return;
        }

        lv_point_t point;
        lv_indev_get_point(indev, &point);

        lv_obj_t *arc = ui->progress_arc;
        lv_coord_t center_x = 0;
        lv_coord_t center_y = 0;

        lv_obj_t *obj = arc;
        while (obj != NULL) {
            center_x += lv_obj_get_x(obj);
            center_y += lv_obj_get_y(obj);
            obj = lv_obj_get_parent(obj);
            if (obj == lv_scr_act()) {
                break;
            }
        }
        center_x += lv_obj_get_width(arc) / 2;
        center_y += lv_obj_get_height(arc) / 2;

        float angle = calculate_angle_from_center(center_x, center_y, point.x, point.y);

        if (angle < 0) {
            angle = 0;
        }
        if (angle > 360) {
            angle = 360;
        }

        update_time_from_angle(ui, (int32_t)angle);

    } else if (code == LV_EVENT_RELEASED) {
        if (ui->timer && ui->remaining_seconds > 0) {
            lv_timer_resume(ui->timer);
        }
    }
}

static void start_knob_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_t *knob = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_set_style_transform_scale(knob, 280, 0);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_t *knob = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_set_style_transform_scale(knob, 256, 0);
    }
}

static void toggle_btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Toggle button clicked - switching start/pause");
        alarm_pomodoro_toggle_start_pause();
    }
}

static void timer_tick_cb(lv_timer_t *timer)
{
    alarm_pomodoro_ui_t *ui = (alarm_pomodoro_ui_t *)lv_timer_get_user_data(timer);

    /* Check if current page is pomodoro page */
    const char *current_page = ui_bridge_get_current_page();
    if (current_page == NULL || strcmp(current_page, PAGE_POMODORO) != 0) {
        return;
    }

    if (ui->remaining_seconds > 0) {
        ui->remaining_seconds--;
        // Mark that timer was running (for detecting natural countdown finish)
        ui->was_running_before_zero = true;

        // Update time display
        int minutes = ui->remaining_seconds / 60;
        int seconds = ui->remaining_seconds % 60;
        if (ui->time_min_label && ui->time_sec_label) {
            lv_label_set_text_fmt(ui->time_min_label, "%02d", minutes);
            lv_label_set_text_fmt(ui->time_sec_label, "%02d", seconds);
        }

        // Update arc progress (0-360 degrees, based on 60 minute max)
        int32_t progress = (360 * ui->remaining_seconds) / TIMER_MAX_SECONDS;
        lv_arc_set_angles(ui->progress_arc, 0, progress);

        pomodoro_set_state(ui, TIMER_STATE_RUNNING);

        // Update knob positions
        update_knob_positions(ui);

    } else {
        // Timer finished
        if (ui->time_min_label && ui->time_sec_label) {
            lv_label_set_text(ui->time_min_label, "00");
            lv_label_set_text(ui->time_sec_label, "00");
        }
        pomodoro_set_state(ui, TIMER_STATE_END);
        lv_timer_pause(timer);

        if (ui->was_running_before_zero) {
            ESP_LOGI(TAG, "Pomodoro timer finished");
            alarm_time_up_set_origin(PAGE_POMODORO);
            main_ui_switch_page(PAGE_TIME_UP);
            ui->was_running_before_zero = false;
        } else {
            ESP_LOGI(TAG, "Timer set to 0 manually or initialized to 0 - no jump");
        }
    }
}

lv_obj_t *alarm_pomodoro_create_with_parent(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating pomodoro timer UI");
    s_pomodoro_ui.remaining_seconds = TIMER_INITIAL_SECONDS;
    s_pomodoro_ui.state = (s_pomodoro_ui.remaining_seconds > 0) ? TIMER_STATE_RUNNING : TIMER_STATE_END;
    s_pomodoro_ui.eaf_anim = NULL;
    s_pomodoro_ui.was_running_before_zero = false;

    s_pomodoro_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_pomodoro_ui.container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_pomodoro_ui.container, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_pomodoro_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_pomodoro_ui.container, 0, 0);
    lv_obj_clear_flag(s_pomodoro_ui.container, LV_OBJ_FLAG_SCROLLABLE);

#if 1
    s_pomodoro_ui.eaf_anim = lv_eaf_create(s_pomodoro_ui.container);
    lv_obj_align(s_pomodoro_ui.eaf_anim, LV_ALIGN_CENTER, 0, 50);

    s_eaf_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_eaf_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    s_eaf_dsc.header.flags = 0;
    s_eaf_dsc.header.w = 1;
    s_eaf_dsc.header.h = 1;
    s_eaf_dsc.header.stride = 0;
    s_eaf_dsc.header.reserved_2 = 0;
    s_eaf_dsc.data_size = sizeof(clock_loop_eaf);
    s_eaf_dsc.data = clock_loop_eaf;

    lv_eaf_set_src(s_pomodoro_ui.eaf_anim, &s_eaf_dsc);
    lv_eaf_set_frame_delay(s_pomodoro_ui.eaf_anim, 30);
    lv_eaf_pause(s_pomodoro_ui.eaf_anim);

    int32_t total_frames = lv_eaf_get_total_frames(s_pomodoro_ui.eaf_anim);
    ESP_LOGI(TAG, "Total frames: %d", total_frames);
#endif

    lv_coord_t arc_size = LV_MIN(SCREEN_WIDTH, SCREEN_HEIGHT) - 20;

    s_pomodoro_ui.bg_arc = lv_arc_create(s_pomodoro_ui.container);
    lv_obj_set_size(s_pomodoro_ui.bg_arc, arc_size + 20, arc_size + 20);
    lv_obj_center(s_pomodoro_ui.bg_arc);
    lv_arc_set_rotation(s_pomodoro_ui.bg_arc, 270);
    lv_arc_set_bg_angles(s_pomodoro_ui.bg_arc, 0, 360);
    lv_obj_remove_style(s_pomodoro_ui.bg_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_pomodoro_ui.bg_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pomodoro_ui.bg_arc, 50, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_pomodoro_ui.bg_arc, lv_color_make(0xc6, 0xc6, 0xc6), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pomodoro_ui.bg_arc, 0, LV_PART_INDICATOR);
    lv_arc_set_value(s_pomodoro_ui.bg_arc, 0);

    s_pomodoro_ui.progress_arc = lv_arc_create(s_pomodoro_ui.container);
    lv_obj_set_size(s_pomodoro_ui.progress_arc, arc_size, arc_size);
    lv_obj_center(s_pomodoro_ui.progress_arc);
    lv_arc_set_rotation(s_pomodoro_ui.progress_arc, 270);

    lv_arc_set_mode(s_pomodoro_ui.progress_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(s_pomodoro_ui.progress_arc, 0, 360);
    lv_arc_set_range(s_pomodoro_ui.progress_arc, 0, 360);

    int32_t initial_angle = (360 * TIMER_INITIAL_SECONDS) / TIMER_MAX_SECONDS;
    lv_arc_set_angles(s_pomodoro_ui.progress_arc, 0, initial_angle);

    // ESP_LOGI(TAG, "Initial angle set to: %ld° (from 0° to %ld°) for %ld seconds",
    //          initial_angle, initial_angle, TIMER_INITIAL_SECONDS);

    lv_obj_set_style_arc_image_src(s_pomodoro_ui.progress_arc, &time_arc_texture, LV_PART_INDICATOR);
    lv_obj_remove_style(s_pomodoro_ui.progress_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_pomodoro_ui.progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pomodoro_ui.progress_arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pomodoro_ui.progress_arc, 30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_pomodoro_ui.progress_arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pomodoro_ui.progress_arc, lv_color_make(0xc6, 0xc6, 0xc6), LV_PART_INDICATOR);

    s_pomodoro_ui.time_container = lv_obj_create(s_pomodoro_ui.container);
    lv_obj_set_size(s_pomodoro_ui.time_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_pomodoro_ui.time_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomodoro_ui.time_container, 0, 0);
    lv_obj_set_style_pad_all(s_pomodoro_ui.time_container, 0, 0);
    lv_obj_align(s_pomodoro_ui.time_container, LV_ALIGN_TOP_MID, 0, 87);
    lv_obj_set_size(s_pomodoro_ui.time_container, 160, 70);
    // Grid: 3 columns (FR, CONTENT, FR) and 1 row
    static const int32_t col_dsc[] = {
        LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };
    static const int32_t row_dsc[] = {
        LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_grid_dsc_array(s_pomodoro_ui.time_container, col_dsc, row_dsc);

    // Minutes label in column 0
    s_pomodoro_ui.time_min_label = lv_label_create(s_pomodoro_ui.time_container);
    int init_minutes = s_pomodoro_ui.remaining_seconds / 60;
    int init_seconds = s_pomodoro_ui.remaining_seconds % 60;
    lv_label_set_text_fmt(s_pomodoro_ui.time_min_label, "%02d", init_minutes);
    lv_obj_set_grid_cell(s_pomodoro_ui.time_min_label,
                         LV_GRID_ALIGN_END, 0, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    // Colon label in column 1 (center)
    s_pomodoro_ui.time_colon_label = lv_label_create(s_pomodoro_ui.time_container);
    lv_label_set_text(s_pomodoro_ui.time_colon_label, ":");
    lv_obj_set_grid_cell(s_pomodoro_ui.time_colon_label,
                         LV_GRID_ALIGN_CENTER, 1, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    // Seconds label in column 2
    s_pomodoro_ui.time_sec_label = lv_label_create(s_pomodoro_ui.time_container);
    lv_label_set_text_fmt(s_pomodoro_ui.time_sec_label, "%02d", init_seconds);
    lv_obj_set_grid_cell(s_pomodoro_ui.time_sec_label,
                         LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_set_style_text_font(s_pomodoro_ui.time_min_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_font(s_pomodoro_ui.time_colon_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_font(s_pomodoro_ui.time_sec_label, &lv_font_montserrat_40, 0);

    lv_obj_set_style_text_color(s_pomodoro_ui.time_min_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_pomodoro_ui.time_colon_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_pomodoro_ui.time_sec_label, lv_color_white(), 0);

    // Create knob objects with textures
    // Start knob (at the beginning of the arc) -> time_start
    s_pomodoro_ui.start_knob_img = lv_img_create(s_pomodoro_ui.container);
    lv_img_set_src(s_pomodoro_ui.start_knob_img, &time_start);
    lv_obj_clear_flag(s_pomodoro_ui.start_knob_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_pomodoro_ui.start_knob_img, LV_OBJ_FLAG_CLICKABLE);  // Not draggable
    // Add subtle visual feedback on touch (optional)
    lv_obj_add_event_cb(s_pomodoro_ui.start_knob_img, start_knob_event_handler, LV_EVENT_ALL, &s_pomodoro_ui);

    // End knob (at the current position of the arc) - This is the main draggable knob -> time_end
    s_pomodoro_ui.end_knob_img = lv_img_create(s_pomodoro_ui.container);
    lv_img_set_src(s_pomodoro_ui.end_knob_img, &time_end);
    lv_obj_clear_flag(s_pomodoro_ui.end_knob_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pomodoro_ui.end_knob_img, LV_OBJ_FLAG_CLICKABLE);  // Make it clickable
    // Add event handler for dragging
    lv_obj_add_event_cb(s_pomodoro_ui.end_knob_img, end_knob_event_handler, LV_EVENT_ALL, &s_pomodoro_ui);

    // Force update layout once to ensure arc coordinates/size are final values,
    // avoid getting x/y as 0 on first calculation which would cause knob to move to top-left corner
    lv_obj_update_layout(s_pomodoro_ui.container);
    int32_t init_angle = (360 * s_pomodoro_ui.remaining_seconds) / TIMER_MAX_SECONDS;
    update_time_from_angle(&s_pomodoro_ui, init_angle);

    // Create transparent toggle button for start/pause (centered, covering time display area)
    s_pomodoro_ui.toggle_btn = lv_btn_create(s_pomodoro_ui.container);
    lv_obj_set_size(s_pomodoro_ui.toggle_btn, 200, 100);
    lv_obj_align(s_pomodoro_ui.toggle_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_pomodoro_ui.toggle_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_pomodoro_ui.toggle_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(s_pomodoro_ui.toggle_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_opa(s_pomodoro_ui.toggle_btn, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_pomodoro_ui.toggle_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pomodoro_ui.toggle_btn, toggle_btn_event_handler, LV_EVENT_CLICKED, NULL);

    // Create timer to update countdown
    s_pomodoro_ui.timer = lv_timer_create(timer_tick_cb, TIMER_UPDATE_PERIOD_MS, &s_pomodoro_ui);

    lv_obj_add_flag(s_pomodoro_ui.container, LV_OBJ_FLAG_HIDDEN);

    return s_pomodoro_ui.container;
}

void alarm_pomodoro_adjust_end_point(int32_t delta_minutes)
{
    ESP_LOGI(TAG, "Adjusted end point by %ld minutes, new remaining: %ld seconds",
             delta_minutes, s_pomodoro_ui.remaining_seconds);

    esp_lv_adapter_lock(-1);

    int32_t old_remaining_seconds = s_pomodoro_ui.remaining_seconds;
    int32_t delta_seconds = delta_minutes * 60;
    int32_t new_remaining_seconds = s_pomodoro_ui.remaining_seconds + delta_seconds;

    if (new_remaining_seconds < 0) {
        new_remaining_seconds = 0;
    } else if (new_remaining_seconds > TIMER_MAX_SECONDS) {
        new_remaining_seconds = TIMER_MAX_SECONDS;
    }

    int32_t new_angle = (360 * new_remaining_seconds) / TIMER_MAX_SECONDS;
    update_time_from_angle(&s_pomodoro_ui, new_angle);

    if (old_remaining_seconds == 0 && new_remaining_seconds > 0) {
        if (s_pomodoro_ui.timer) {
            bool is_paused = lv_timer_get_paused(s_pomodoro_ui.timer);
            if (is_paused) {
                lv_timer_resume(s_pomodoro_ui.timer);
                pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_RUNNING);
                ESP_LOGI(TAG, "Timer auto-started (time changed from 0 to %ld seconds)", new_remaining_seconds);
            }
        }
    }

    esp_lv_adapter_unlock();
}

void alarm_pomodoro_toggle_start_pause(void)
{
    if (!s_pomodoro_ui.timer) {
        ESP_LOGW(TAG, "Timer not initialized");
        return;
    }

    /* Check if current page is pomodoro page, if not, switch to it first */
    const char *current_page = ui_bridge_get_current_page();
    if (current_page == NULL || strcmp(current_page, PAGE_POMODORO) != 0) {
        ESP_LOGI(TAG, "Not on pomodoro page (current=%s), switching to pomodoro page", current_page ? current_page : "NULL");
        main_ui_switch_page(PAGE_POMODORO);
    }

    esp_lv_adapter_lock(-1);
    bool is_paused = lv_timer_get_paused(s_pomodoro_ui.timer);

    if (is_paused) {
        if (s_pomodoro_ui.remaining_seconds > 0) {
            lv_timer_resume(s_pomodoro_ui.timer);
            pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_RUNNING);
            ESP_LOGI(TAG, "Timer resumed");
        } else {
            ESP_LOGI(TAG, "Timer cannot resume: no remaining time");
        }
    } else {
        pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_PAUSED);
        lv_timer_pause(s_pomodoro_ui.timer);
        ESP_LOGI(TAG, "Timer paused");
    }
    esp_lv_adapter_unlock();
}

void alarm_pomodoro_reset_to_zero(void)
{
    ESP_LOGI(TAG, "Timer reset to 0:00");
    esp_lv_adapter_lock(-1);

    if (s_pomodoro_ui.timer) {
        lv_timer_pause(s_pomodoro_ui.timer);
    }

    s_pomodoro_ui.remaining_seconds = 0;
    s_pomodoro_ui.was_running_before_zero = false;
    update_time_from_angle(&s_pomodoro_ui, 0);
    pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_END);

    esp_lv_adapter_unlock();
}

bool alarm_pomodoro_is_paused(void)
{
    if (!s_pomodoro_ui.timer) {
        return true;  /* Not initialized, consider as paused */
    }

    esp_lv_adapter_lock(-1);
    bool is_paused = lv_timer_get_paused(s_pomodoro_ui.timer);
    esp_lv_adapter_unlock();

    return is_paused;
}

void alarm_pomodoro_start(void)
{
    if (!s_pomodoro_ui.timer) {
        ESP_LOGW(TAG, "Timer not initialized");
        return;
    }

    esp_lv_adapter_lock(-1);
    bool is_paused = lv_timer_get_paused(s_pomodoro_ui.timer);

    if (is_paused) {
        if (s_pomodoro_ui.remaining_seconds > 0) {
            lv_timer_resume(s_pomodoro_ui.timer);
            pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_RUNNING);
            ESP_LOGI(TAG, "Timer started/resumed");
        } else {
            ESP_LOGI(TAG, "Timer cannot start: no remaining time");
        }
    } else {
        ESP_LOGI(TAG, "Timer is already running");
    }
    esp_lv_adapter_unlock();
}

void alarm_pomodoro_pause(void)
{
    if (!s_pomodoro_ui.timer) {
        ESP_LOGW(TAG, "Timer not initialized");
        return;
    }

    esp_lv_adapter_lock(-1);
    bool is_paused = lv_timer_get_paused(s_pomodoro_ui.timer);

    if (!is_paused) {
        pomodoro_set_state(&s_pomodoro_ui, TIMER_STATE_PAUSED);
        lv_timer_pause(s_pomodoro_ui.timer);
        ESP_LOGI(TAG, "Timer paused");
    } else {
        ESP_LOGI(TAG, "Timer is already paused");
    }
    esp_lv_adapter_unlock();
}
