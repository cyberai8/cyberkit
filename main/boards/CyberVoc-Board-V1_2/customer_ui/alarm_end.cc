/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string>
#include <string.h>
#include "alarm_manager.h"
#include "alarm_api.h"
#include "alarm_sleep_24h.h"
#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_config.h"

static const char *TAG = "time_up";

typedef struct {
    lv_obj_t *container;
    lv_obj_t *remind_later_btn;
    lv_obj_t *remind_later_time_label;  /* Time label above remind later button */
    lv_obj_t *slider_container;
    lv_obj_t *slider;
    lv_obj_t *slider_label;
    const char *origin_page;  /* Page that triggered the time up (for navigation back) */
} alarm_time_up_ui_t;

static alarm_time_up_ui_t s_time_up_ui;

static void remind_later_btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Snooze 5 min button clicked");

        if (s_time_up_ui.origin_page != NULL) {
            if (strcmp(s_time_up_ui.origin_page, PAGE_SLEEP) == 0) {
                alarm_sleep_24h_snooze(5);
            } else if (strcmp(s_time_up_ui.origin_page, PAGE_POMODORO) == 0) {
                alarm_start_pomodoro(5);
            }
            s_time_up_ui.origin_page = NULL;  /* Clear origin after use */
        } else {
            main_ui_switch_page(UI_BRIDGE_PAGE_HOME);
        }
    }
}

static void slider_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        int32_t value = lv_slider_get_value(slider);
        if (value >= 100) {
            lv_slider_set_value(slider, 0, LV_ANIM_ON);
            main_ui_switch_page(UI_BRIDGE_PAGE_HOME);
        }
    } else if (code == LV_EVENT_RELEASED) {
        int32_t value = lv_slider_get_value(slider);
        if (value < 100) {
            lv_slider_set_value(slider, 0, LV_ANIM_ON);
        }
    }
}

void alarm_time_up_set_origin(const char *origin_page)
{
    ESP_LOGI(TAG, "Set time up origin page: %s", origin_page ? origin_page : "NULL");
    s_time_up_ui.origin_page = origin_page;

    if (origin_page != NULL && strcmp(origin_page, PAGE_SLEEP) == 0) {
        int32_t end_hour, end_min;
        if (alarm_sleep_24h_get_end_time(&end_hour, &end_min)) {
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02ld:%02ld", (long)end_hour, (long)end_min);

            if (s_time_up_ui.remind_later_time_label) {
                lv_label_set_text(s_time_up_ui.remind_later_time_label, time_str);
                lv_obj_clear_flag(s_time_up_ui.remind_later_time_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        std::string wake_word = Lang::Strings::CLOCK;
        auto& app = Application::GetInstance();
        //app.TriggerWakeWord(wake_word);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app.PlaySound(Lang::Sounds::OGG_ALARM);
    } else if (origin_page != NULL && strcmp(origin_page, PAGE_POMODORO) == 0) {
        if (s_time_up_ui.remind_later_time_label) {
            lv_obj_add_flag(s_time_up_ui.remind_later_time_label, LV_OBJ_FLAG_HIDDEN);
        }
        std::string wake_word = Lang::Strings::CLOCK;
        auto& app = Application::GetInstance();
        //app.TriggerWakeWord(wake_word);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app.PlaySound(Lang::Sounds::OGG_ALARM);
    }
}

lv_obj_t *alarm_time_up_create_with_parent(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating alarm time up UI");

    s_time_up_ui.origin_page = NULL;  /* Initialize to NULL */
    s_time_up_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_time_up_ui.container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_time_up_ui.container, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_time_up_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_time_up_ui.container, 0, 0);

    s_time_up_ui.remind_later_btn = lv_btn_create(s_time_up_ui.container);
    lv_obj_set_size(s_time_up_ui.remind_later_btn, 200, 50);
    lv_obj_align(s_time_up_ui.remind_later_btn, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_radius(s_time_up_ui.remind_later_btn, 25, 0);
    lv_obj_set_style_bg_color(s_time_up_ui.remind_later_btn, lv_color_make(0x98, 0x98, 0x98), 0);
    lv_obj_set_style_bg_opa(s_time_up_ui.remind_later_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_time_up_ui.remind_later_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_time_up_ui.remind_later_btn, 0, 0);
    lv_obj_set_style_shadow_opa(s_time_up_ui.remind_later_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_time_up_ui.remind_later_btn, remind_later_btn_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *remind_label = lv_label_create(s_time_up_ui.remind_later_btn);
    lv_label_set_text(remind_label, "Snooze 5 min");
    lv_obj_set_style_text_font(remind_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(remind_label, lv_color_white(), 0);
    lv_obj_center(remind_label);

    /* Create time label above remind later button */
    s_time_up_ui.remind_later_time_label = lv_label_create(s_time_up_ui.container);
    lv_label_set_text(s_time_up_ui.remind_later_time_label, "");
    lv_obj_set_style_text_font(s_time_up_ui.remind_later_time_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_time_up_ui.remind_later_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_up_ui.remind_later_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_up_ui.remind_later_time_label, LV_ALIGN_CENTER, 0, -120);
    lv_obj_add_flag(s_time_up_ui.remind_later_time_label, LV_OBJ_FLAG_HIDDEN);

    s_time_up_ui.slider_container = lv_obj_create(s_time_up_ui.container);
    lv_obj_set_size(s_time_up_ui.slider_container, 260, 80);
    lv_obj_align(s_time_up_ui.slider_container, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_bg_opa(s_time_up_ui.slider_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_time_up_ui.slider_container, 0, 0);
    lv_obj_set_style_border_width(s_time_up_ui.slider_container, 0, 0);
    lv_obj_set_style_pad_all(s_time_up_ui.slider_container, 0, 0);

    s_time_up_ui.slider_label = lv_label_create(s_time_up_ui.slider_container);
    lv_label_set_text(s_time_up_ui.slider_label, "Slide to Stop");
    lv_obj_set_style_text_font(s_time_up_ui.slider_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_time_up_ui.slider_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_up_ui.slider_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_up_ui.slider_label, LV_ALIGN_CENTER, 0, 0);

    s_time_up_ui.slider = lv_slider_create(s_time_up_ui.slider_container);
    lv_obj_set_width(s_time_up_ui.slider, 200);
    lv_obj_set_height(s_time_up_ui.slider, 50);
    lv_obj_clear_flag(s_time_up_ui.slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_time_up_ui.slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_width(s_time_up_ui.slider, 40, LV_PART_KNOB);
    lv_obj_set_style_height(s_time_up_ui.slider, 40, LV_PART_KNOB);
    lv_obj_set_style_radius(s_time_up_ui.slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_time_up_ui.slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_time_up_ui.slider, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_img_src(s_time_up_ui.slider, &time_sleep, LV_PART_KNOB);
    lv_slider_set_range(s_time_up_ui.slider, 0, 100);
    lv_slider_set_value(s_time_up_ui.slider, 0, LV_ANIM_OFF);

    lv_obj_set_style_radius(s_time_up_ui.slider, 25, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_time_up_ui.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_time_up_ui.slider, lv_color_make(0x3F, 0x3F, 0x3F), LV_PART_MAIN);

    lv_obj_set_style_radius(s_time_up_ui.slider, 25, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_time_up_ui.slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_time_up_ui.slider, lv_color_make(0x98, 0x98, 0x98), LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s_time_up_ui.slider, lv_color_make(0x41, 0x41, 0x41), LV_PART_KNOB);
    lv_obj_add_event_cb(s_time_up_ui.slider, slider_event_handler, LV_EVENT_ALL, NULL);

    lv_obj_move_foreground(s_time_up_ui.slider_label);

    lv_obj_add_flag(s_time_up_ui.container, LV_OBJ_FLAG_HIDDEN);

    return s_time_up_ui.container;
}
