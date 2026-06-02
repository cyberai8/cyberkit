/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "config.h"
#include "touch_button.h"
#include "iot_button.h"
#include "touch_sensor.h"
#include "touch_sensor_lowlevel.h"
#include "board.h"
#include "display/display.h"
#include "assets/lang_config.h"
#include "application.h"
#include "ui_bridge.h"
#include "esp32_music.h"

#include "assets/lang_config.h"
extern "C" { // temporary solution for eliminate compilation errors
#include "touch_slider_sensor.h"
}

const static char *TAG = "Touch";

static uint32_t touch_channel_list[1] = { // define touch channels

};

touch_lowlevel_type_t channel_type[] = {
    TOUCH_LOWLEVEL_TYPE_TOUCH, 
};

// Touch button handles for multi-tap gestures
static button_handle_t touch_btn_handle[] = {
    NULL, 
};

// Touch button event callback
static void touch_btn_event_cb(void *button_handle, void *usr_data)
{
    button_event_t event = iot_button_get_event((button_handle_t)button_handle);
    switch (event) {
    case BUTTON_PRESS_DOWN:
        ESP_LOGD(TAG, "Press down");
        break;
    case BUTTON_PRESS_UP:
        ESP_LOGD(TAG, "Press up");
        break;
    case BUTTON_SINGLE_CLICK:
        ESP_LOGD(TAG, "Single click");
        break;
    case BUTTON_LONG_PRESS_START: {
        {
            ESP_LOGI(TAG, "Long press started");
            Display* display = Board::GetInstance().GetDisplay();
            if (display == nullptr) {
                return;
            }
            auto music = Board::GetInstance().GetMusic();  
            bool music_play = music->MusicPlaying();
            ESP_LOGI(TAG, "当前放歌状态：%s", music_play ? "是" : "否");
            
            if(ui_bridge_is_on_home_page())
            {
                auto &app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateListening || app.GetDeviceState() == kDeviceStateSpeaking) {
                    
                    if(Board::GetInstance().GetTouchSensor()->get_touch())
                    {
                        if(music_play){
                            music->StopStreaming();
                        }
                        vTaskDelay(pdMS_TO_TICKS(300));
                       app.TriggerWakeWord(Lang::Strings::TOUCH_HEAD); 
                    }
                    
                }
                else if(app.GetDeviceState() == kDeviceStateIdle)
                {
                    if(Board::GetInstance().GetTouchSensor()->get_touch())
                    {
                        if(music_play){
                            music->StopStreaming();
                        }
                        vTaskDelay(pdMS_TO_TICKS(300));
                        std::string wake_word = Lang::Strings::TOUCH_HEAD;
                        app.WakeWordInvoke(wake_word);
                    }
                }
            }
            
            break;
        }
    }
    default:
        break;
    }
}

#if TOUCH_SLIDER_ENABLED
// Touch gesture coordination variables (for shared channels 6&7)
static bool is_sliding_detected = false;
static touch_slider_handle_t touch_slider_handle = NULL;
#endif

static esp_err_t init_touch_button(void)
{
    uint32_t channel_num = sizeof(touch_channel_list) / sizeof(touch_channel_list[0]);
    ESP_LOGI(TAG, "touch channel num: %ld\n", channel_num);
    touch_lowlevel_config_t low_config = {
        .channel_num = channel_num,
        .channel_list = (uint32_t *)touch_channel_list,
        .channel_type = channel_type,
    };

    esp_err_t ret = touch_sensor_lowlevel_create(&low_config);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create touch sensor lowlevel");

    // Touch button configuration (shared by both buttons)
    const button_config_t btn_cfg = {
        .long_press_time = 100,  // Long press time in ms
        .short_press_time = 50,  // Short press time in ms
    };

    for (size_t i = 0; i < channel_num; i++) {
        button_touch_config_t touch_cfg_1 = {
            .touch_channel = (int32_t)touch_channel_list[i],
            .channel_threshold = 0.05, // Touch threshold (adjust as needed)
                                  .skip_lowlevel_init = true,
        };
        ESP_LOGI(TAG, "Touch button %d channel: %d", i + 1, touch_channel_list[i]);
        // Create first touch button device
        ret = iot_button_new_touch_button_device(&btn_cfg, &touch_cfg_1, &touch_btn_handle[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create touch button 1 device: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    touch_sensor_lowlevel_start();

    // Register button event callbacks
    ESP_LOGI(TAG, "Registering button event callbacks for handle: %p", touch_btn_handle[0]);
    iot_button_register_cb(touch_btn_handle[0], BUTTON_PRESS_UP, NULL, touch_btn_event_cb, NULL);
    iot_button_register_cb(touch_btn_handle[0], BUTTON_DOUBLE_CLICK, NULL, touch_btn_event_cb, NULL);
    iot_button_register_cb(touch_btn_handle[0], BUTTON_SINGLE_CLICK, NULL, touch_btn_event_cb, NULL);
    iot_button_register_cb(touch_btn_handle[0], BUTTON_LONG_PRESS_START, NULL, touch_btn_event_cb, NULL);
    return ESP_OK;
}

#if TOUCH_SLIDER_ENABLED
static void touch_slider_callback(touch_slider_handle_t handle, touch_slider_event_t event, int32_t data, void *cb_arg)
{
    switch (event) {
    case TOUCH_SLIDER_EVENT_POSITION:
        {ESP_LOGD(TAG, "Position event ignored to prevent button conflicts");
        break;
}
    case TOUCH_SLIDER_EVENT_RIGHT_SWIPE:
        {if (!is_sliding_detected) {
            is_sliding_detected = true;
            ESP_LOGI(TAG, "Swipe detected, taking control from buttons");
        }
        ESP_LOGI(TAG, "Right swipe");
        break;}

    case TOUCH_SLIDER_EVENT_LEFT_SWIPE:
        {if (!is_sliding_detected) {
            is_sliding_detected = true;
            ESP_LOGI(TAG, "Swipe detected, taking control from buttons");
        }
        ESP_LOGI(TAG, "Left swipe");
        break;}

    case TOUCH_SLIDER_EVENT_RELEASE:
       { 
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateListening || app.GetDeviceState() == kDeviceStateSpeaking) {
            app.TriggerWakeWord(Lang::Strings::TOUCH_HEAD);
        }
        else if(app.GetDeviceState() == kDeviceStateIdle)
        {
            std::string wake_word = Lang::Strings::TOUCH_HEAD;
            app.WakeWordInvoke(wake_word);
            ESP_LOGI(TAG, "swipe detected: %s",
                    is_sliding_detected ? "YES" : "NO");

        }
        
        // Reset gesture state
        is_sliding_detected = false;
        break;}

    default:
        break;
    }
}

// Task function to handle touch slider events
static void touch_slider_task(void *pvParameters)
{
    touch_slider_handle_t handle = (touch_slider_handle_t)pvParameters;
    ESP_LOGI(TAG, "Touch slider control task started");
    while (1) {
        if (touch_slider_sensor_handle_events(handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to handle touch slider events");
        }
        vTaskDelay(pdMS_TO_TICKS(20));  // 20ms polling interval
    }
}

// Initialize touch slider control
static esp_err_t init_touch_slider(void)
{
    // Touch slider configuration - sharing channels 6&7 with button system
    float threshold[] = {0.015f, 0.015f};  // Touch thresholds for each channel
    uint32_t channel_num = sizeof(touch_channel_list) / sizeof(touch_channel_list[0]);

    // Configure touch slider for swipe-only control
    touch_slider_config_t config = {
        .channel_num = channel_num,
        .channel_list = (uint32_t *)touch_channel_list,
        .channel_threshold = threshold,
        .channel_gold_value = NULL,
        .debounce_times = 1,            // Reduced debounce for faster response
        .filter_reset_times = 2,        // Reduced for faster response
        .position_range = 100,          // Simple volume range 0-100
        .calculate_window = 2,
        .swipe_threshold = 4.0f,       // Lower threshold for easier swipe detection
        .swipe_hysterisis = 2.0f,      // Lower hysteresis for better responsiveness
        .swipe_alpha = 0.3f,            // Slightly less smoothing for more responsive swipes
        .skip_lowlevel_init = true,     // Use existing lowlevel init from touch buttons
    };

    // Create touch slider sensor
    esp_err_t ret = touch_slider_sensor_create(&config, &touch_slider_handle, touch_slider_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch slider sensor: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create task to handle touch events
    BaseType_t task_ret = xTaskCreate(touch_slider_task, "touchslider_task", 4096, touch_slider_handle, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch slider task");
        touch_slider_sensor_delete(touch_slider_handle);
        touch_slider_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Touch slider control initialized successfully");
    return ESP_OK;
}

#endif

TouchSensor::TouchSensor()
{

}

TouchSensor::~TouchSensor()
{

}

bool TouchSensor::init(uint8_t pcb_verison)
{
    if(pcb_verison == 0)
    {
        touch_channel_list[0] = TOUCH_PAD1;
    }
    else if(pcb_verison == 1)
    {
        touch_channel_list[0] = TOUCH_PAD2_2;
    }
    esp_err_t ret = init_touch_button();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize touch button: %s", esp_err_to_name(ret));
        return false;
    }
#if TOUCH_SLIDER_ENABLED
    ret = init_touch_slider();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize touch slider: %s", esp_err_to_name(ret));
        return false;
    }
#endif
    return true;
}

button_handle_t TouchSensor::get_button_handle()
{
    return touch_btn_handle[0];
}