#include "touch_cst816s.h"
#include "config.h"
#include <esp_log.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lcd_panel_io.h>

#define TAG "Cst816s"

bool Cst816sTouch::init(int width,
                        int height,
                        bool swap_xy,
                        bool mirror_x,
                        bool mirror_y)
{
    ESP_LOGI(TAG, "Initializing CST816S touch controller");

    if (i2c_bus_ == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL");
        return false;
    }

    // Configure touch panel parameters
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = static_cast<uint16_t>(width),
        .y_max = static_cast<uint16_t>(height),
        .rst_gpio_num = TP_PIN_NUM_RST,
        .int_gpio_num = TP_PIN_NUM_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
    };

    // Create panel IO handle for touch controller
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400000,
    };
    
    esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO for touch controller: %s", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize CST816S touch controller
    ret = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CST816S touch driver: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "CST816S touch controller initialized successfully");
    return true;
}
