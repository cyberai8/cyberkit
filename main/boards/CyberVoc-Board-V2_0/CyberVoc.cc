#include "CyberVoc.h"
#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "touch_cst816s.h"
#include <esp_lcd_touch.h>
#include <esp_lv_adapter.h>
#include <lvgl.h>
#include "base_control.h"
#include "audio_analysis.h"
#include "cybervoc_tools.h"
#include "touch_sensor.h"
#include "ui_bridge.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
//#include <driver/i2c.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_psram.h>
#include <esp_lcd_st77916.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <driver/temperature_sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <functional>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"//SD卡相关头文件
#include "driver/sdmmc_host.h"

#include "assets/lang_config.h"

#define TAG "CyberVoc"

temperature_sensor_handle_t temp_sensor = NULL;
float tsens_value;
static constexpr gpio_num_t kAudioI2sDin = AUDIO_I2S_GPIO_DIN;
static constexpr gpio_num_t kAudioPaPin = AUDIO_CODEC_PA_PIN;
static constexpr gpio_num_t kLcdRstPin = QSPI_PIN_NUM_LCD_RST;

// ST77916 LCD initialization commands
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0}, {0xF2, (uint8_t []){0x28}, 1, 0}, {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0}, {0x83, (uint8_t []){0xE0}, 1, 0}, {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0}, {0xF0, (uint8_t []){0x00}, 1, 0}, {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0}, {0xB0, (uint8_t []){0x56}, 1, 0}, {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0}, {0xB4, (uint8_t []){0x87}, 1, 0}, {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0}, {0xB7, (uint8_t []){0x40}, 1, 0}, {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0}, {0xBB, (uint8_t []){0x08}, 1, 0}, {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0}, {0xC0, (uint8_t []){0x80}, 1, 0}, {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0}, {0xC3, (uint8_t []){0x80}, 1, 0}, {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0}, {0xC6, (uint8_t []){0xA9}, 1, 0}, {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0}, {0xC9, (uint8_t []){0xA9}, 1, 0}, {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0}, {0xD0, (uint8_t []){0x91}, 1, 0}, {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0}, {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0}, {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0}, {0xF1, (uint8_t []){0x10}, 1, 0}, {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0}, {0xF3, (uint8_t []){0x10}, 1, 0}, {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0}, {0xE2, (uint8_t []){0x00}, 1, 0}, {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0}, {0xE5, (uint8_t []){0x06}, 1, 0}, {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0}, {0xE8, (uint8_t []){0x05}, 1, 0}, {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0}, {0xEB, (uint8_t []){0x00}, 1, 0}, {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0}, {0xEE, (uint8_t []){0x00}, 1, 0}, {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0}, {0xF9, (uint8_t []){0x00}, 1, 0}, {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0}, {0xFC, (uint8_t []){0x00}, 1, 0}, {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0}, {0xFF, (uint8_t []){0x00}, 1, 0}, {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0}, {0x62, (uint8_t []){0x00}, 1, 0}, {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0}, {0x65, (uint8_t []){0x00}, 1, 0}, {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0}, {0x68, (uint8_t []){0x00}, 1, 0}, {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0}, {0x6B, (uint8_t []){0x00}, 1, 0}, {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0}, {0x72, (uint8_t []){0x00}, 1, 0}, {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0}, {0x75, (uint8_t []){0x00}, 1, 0}, {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0}, {0x78, (uint8_t []){0x00}, 1, 0}, {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0}, {0x7B, (uint8_t []){0x00}, 1, 0}, {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0}, {0x82, (uint8_t []){0x06}, 1, 0}, {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0}, {0x85, (uint8_t []){0x04}, 1, 0}, {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0}, {0x88, (uint8_t []){0x48}, 1, 0}, {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0}, {0x8B, (uint8_t []){0x02}, 1, 0}, {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0}, {0x8E, (uint8_t []){0x00}, 1, 0}, {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0}, {0x91, (uint8_t []){0x00}, 1, 0}, {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0}, {0x94, (uint8_t []){0xDA}, 1, 0}, {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0}, {0x97, (uint8_t []){0x00}, 1, 0}, {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0}, {0x9A, (uint8_t []){0x0C}, 1, 0}, {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0}, {0x9D, (uint8_t []){0x04}, 1, 0}, {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0}, {0xA0, (uint8_t []){0x48}, 1, 0}, {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0}, {0xA3, (uint8_t []){0x02}, 1, 0}, {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0}, {0xA6, (uint8_t []){0x00}, 1, 0}, {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0}, {0xA9, (uint8_t []){0x00}, 1, 0}, {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0}, {0xAC, (uint8_t []){0xD7}, 1, 0}, {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0}, {0xAF, (uint8_t []){0x00}, 1, 0}, {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0}, {0xB2, (uint8_t []){0x09}, 1, 0}, {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0}, {0xB5, (uint8_t []){0x04}, 1, 0}, {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0}, {0xB8, (uint8_t []){0x48}, 1, 0}, {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0}, {0xBB, (uint8_t []){0x02}, 1, 0}, {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0}, {0xBE, (uint8_t []){0x00}, 1, 0}, {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0}, {0xC1, (uint8_t []){0x47}, 1, 0}, {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0}, {0xC4, (uint8_t []){0x74}, 1, 0}, {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0}, {0xC7, (uint8_t []){0x01}, 1, 0}, {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0}, {0xD0, (uint8_t []){0x10}, 1, 0}, {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0}, {0xD3, (uint8_t []){0x65}, 1, 0}, {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0}, {0xD6, (uint8_t []){0x99}, 1, 0}, {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0}, {0xD9, (uint8_t []){0xAA}, 1, 0}, {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0}, {0x21, (uint8_t []){}, 0, 0}, {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};

//电量部分
static const parameter_cedv_t default_cedv = {
    .full_charge_cap = 1200,    /* 额定容量1200mAh */
    .design_cap = 1200,         /* 设计容量1200mAh */
    .reserve_cap = 60,          /* 储备容量，约5%的额定容量 */
    .near_full = 1140,          /* 接近充满阈值，95%容量 */
    .self_discharge_rate = 8,   /* 自放电率：8 × 0.0025% = 0.02%/天 */
    
    /* EDV参数 - 基于放电终止电压3.2V，但调整上限为4.1V */
    .EDV0 = 3200,               /* 0% SOC - 完全放空电压 */
    .EDV1 = 3300,               /* 3% SOC */
    .EDV2 = 3400,               /* 电池低电量警告电压 */
    
    /* 电化学参数调整 */
    .EMF = 4050,                /* 空载电压，调整为略低于4.1V充满电压 */
    .C0 = 1400,                 /* 稍小的容量相关EDV调整因子（电压范围变小） */
    .R0 = 55,                   /* 稍大的阻抗调整因子（电压窗口变小） */
    .T0 = 100,                  /* 温度阻抗变化因子 */
    .R1 = 28,                   /* 稍大的容量阻抗变化因子 */
    .TC = 125,                  /* 冷温度阻抗调整 */
    .C1 = 35,                   /* EDV0时保留容量稍大为35mAh */
    
    /* DOD电压曲线 - 基于3.2V-4.1V范围重新调整 */
    .DOD0 = 4100,               /* 0% DOD - 充满4.1V */
    .DOD10 = 4030,              /* 10% DOD - 4.03V */
    .DOD20 = 3970,              /* 20% DOD - 3.97V */
    .DOD30 = 3910,              /* 30% DOD - 3.91V */
    .DOD40 = 3850,              /* 40% DOD - 3.85V */
    .DOD50 = 3800,              /* 50% DOD - 3.80V */
    .DOD60 = 3750,              /* 60% DOD - 3.75V */
    .DOD70 = 3700,              /* 70% DOD - 3.70V */
    .DOD80 = 3620,              /* 80% DOD - 3.62V */
    .DOD90 = 3520,              /* 90% DOD - 3.52V */
    .DOD100 = 3200              /* 100% DOD - 放空3.2V */
};

static const gauging_config_t default_config = {
    .CCT = 1,        /* 使用充满容量百分比 */
    .CSYNC = 1,      /* 启用容量同步 */
    .EDV_CMP = 1,    /* 启用EDV补偿 */
    .SC = 1,         /* 启用短路检测 */
    .FIXED_EDV0 = 0, /* 不固定EDV0 */
    .FCC_LIM = 1,    /* 启用充满容量限制 */
    .FC_FOR_VDQ = 0, /* 不为VDQ使用充满电压 */
    .IGNORE_SD = 0,  /* 不忽略自放电 */
    .SME0 = 0        /* 禁用特定模式 */
};


// PCB version dependent GPIO pins are now defined via macros in config.h

void EspS3Cat::InitializeI2c()
{
    // i2c_master_bus_config_t i2c_bus_cfg = {
    //     .i2c_port = I2C_NUM_0,
    //     .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
    //     .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
    //     .clk_source = I2C_CLK_SRC_DEFAULT,
    //     .glitch_ignore_cnt = 7,
    //     .intr_priority = 0,
    //     .trans_queue_depth = 0,
    //     .flags = {
    //         .enable_internal_pullup = 1,
    //     },
    // };
    // ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    // i2c_bus_ = i2c_bus_get_internal_bus_handle(i2c_bus);

    // temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    // ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
    // ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
    i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master = {
                .clk_speed = 400 * 1000
            },
            .clk_flags = 0  // 或者 I2C_SCLK_SRC_FLAG_AWARE_DFS | I2C_SCLK_SRC_FLAG_LIGHT_SLEEP
        };
        i2c_bus = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
        i2c_bus_ = i2c_bus_get_internal_bus_handle(i2c_bus);

    vTaskDelay(pdMS_TO_TICKS(50));

    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
    ESP_LOGI(TAG, "I2C initialized"); 
}

// PCB version detection removed - now using SELECT_BOARD_VERSION macro in config.h

void EspS3Cat::InitializeSpi()
{
    const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                              QSPI_PIN_NUM_LCD_DATA0,
                                                                              QSPI_PIN_NUM_LCD_DATA1,
                                                                              QSPI_PIN_NUM_LCD_DATA2,
                                                                              QSPI_PIN_NUM_LCD_DATA3,
                                                                              DISPLAY_WIDTH * 1 * sizeof(uint16_t),
                                                                              static_cast<esp_intr_cpu_affinity_t>(LCD_CORE)
                                                                            );//DISPLAY_WIDTH * 8 * sizeof(uint16_t)
    ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
}

void start_lvgl(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                Display *display)
{
    lv_init();

#if CONFIG_SPIRAM
// lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initializing LVGL adapter, width:%d, height:%d", width, height);
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = 4;
    adapter_config.task_core_id = 1;
    adapter_config.tick_period_ms = 50;
    adapter_config.task_min_delay_ms = 10;
    adapter_config.task_max_delay_ms = 1000;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                                                         panel,
                                                         panel_io,
                                                         static_cast<uint16_t>(width),
                                                         static_cast<uint16_t>(height),
                                                         ESP_LV_ADAPTER_ROTATE_0);
    display_config.profile.use_psram = true;
    display_config.profile.buffer_height = 20;
    display_config.profile.require_double_buffer = false;

    lv_display_t *display_ = esp_lv_adapter_register_display(&display_config);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    ESP_LOGI(TAG, "Starting LVGL adapter");
    esp_lv_adapter_set_dummy_draw(display_, true);
    esp_lv_adapter_start();

    esp_lv_adapter_lock(-1);
    /* Pass the display pointer directly to avoid Board::GetInstance() call */
    ui_bridge_init(display);
    esp_lv_adapter_unlock();
}

void EspS3Cat::Initializest77916Display(uint8_t pcb_verison)
{
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    (void)pcb_verison;

    // Ported from esp-vocat: lower QSPI clock + deeper queue for stability under load.
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num        = QSPI_PIN_NUM_LCD_CS,
        .dc_gpio_num        = -1,
        .spi_mode           = 0,
        .pclk_hz            = 12 * 1000 * 1000,
        .trans_queue_depth  = 16,
        .on_color_trans_done = nullptr,
        .user_ctx           = nullptr,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
    st77916_vendor_config_t vendor_config = {
        .init_cmds = vendor_specific_init_yysj,
        .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = kLcdRstPin,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
        .flags = {
            .reset_active_high = false,
        },
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
    display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    start_lvgl(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, display_);
#else
    display_ = new SpiLcdDisplay(panel_io, panel,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    backlight_->RestoreBrightness();
}

void EspS3Cat::InitializeButtons()
{
    boot_button_.OnClick([this]() {
        ESP_LOGI(TAG, "屏幕click");
        if (ui_bridge_is_on_home_page()) {
            auto &app = Application::GetInstance();
            
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                ESP_LOGI(TAG, "Boot button pressed, switch network type");
                SwitchNetworkType();
                return;
            }

            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
            }
            app.WakeWordInvoke("你好");
            app.ToggleChatState();
        }
    });

    boot_button_.OnDoubleClick([this]() {
        ESP_LOGI(TAG, "BOOT button double click");
        if (!ui_bridge_is_on_home_page()) {
            return;
        }
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            SwitchNetworkType();
        }
    });

    boot_button_.OnMultipleClick([this]() {
        ESP_LOGI(TAG, "BOOT button triple click");
        if (!ui_bridge_is_on_home_page()) {
            return;
        }
        // 目前处于WIFI时，连续三击进入配网
        if (GetNetworkType() == NetworkType::WIFI) {
            EnterWifiConfigMode();
        }
    }, 3);
}

void EspS3Cat::PowerOff()
{
    ESP_LOGW(TAG, "Power OFF: releasing PG2(GPIO%d)", PG2_HOLD_GPIO);
    ShowPowerOverlay("设备即将关机");
    vTaskDelay(pdMS_TO_TICKS(800));
    gpio_set_level(PG2_HOLD_GPIO, !PG2_HOLD_ACTIVE_LEVEL);

    // If USB/debug power still feeds the MCU, the latch release will not fully cut rails.
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "Latch released — if rails stay up (USB/debug), unplug to fully power down");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void EspS3Cat::ShowPowerOverlay(const char* message)
{
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display_);
    if (emote_display != nullptr) {
        emote_display->ShowPowerOverlay(message);
    }
}

void EspS3Cat::HidePowerOverlay()
{
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display_);
    if (emote_display != nullptr) {
        emote_display->HidePowerOverlay();
    }
}

static void power_key_task(void* arg)
{
    auto* self = static_cast<EspS3Cat*>(arg);
    int accumulated_ms = 0;
    bool wait_release_before_shutdown = gpio_get_level(PG1_POWER_KEY_GPIO) == PG1_POWER_KEY_ACTIVE_LEVEL;
    if (wait_release_before_shutdown) {
        ESP_LOGI(TAG, "[POWER] Release PG1(GPIO%d) once to arm single-click shutdown",
                 PG1_POWER_KEY_GPIO);
    }

    while (true) {
        const bool pressed = gpio_get_level(PG1_POWER_KEY_GPIO) == PG1_POWER_KEY_ACTIVE_LEVEL;
        if (wait_release_before_shutdown) {
            if (!pressed) {
                wait_release_before_shutdown = false;
                accumulated_ms = 0;
                ESP_LOGI(TAG, "[POWER] Single-click shutdown armed");
            }
            vTaskDelay(pdMS_TO_TICKS(KEY_GPIO_POLL_MS));
            continue;
        }

        if (pressed) {
            accumulated_ms += KEY_GPIO_POLL_MS;
            if (accumulated_ms >= KEY_SW1_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "[POWER] Long press shutdown (%d ms)", KEY_SW1_LONG_PRESS_MS);
                self->PowerOff();
                accumulated_ms = 0;
            }
        } else {
            if (accumulated_ms > 0 && accumulated_ms < KEY_SW1_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "[POWER] Single click shutdown");
                self->PowerOff();
            }
            accumulated_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_GPIO_POLL_MS));
    }
}

void EspS3Cat::InitializePowerLatch()
{
    gpio_config_t hold_conf = {};
    hold_conf.intr_type = GPIO_INTR_DISABLE;
    hold_conf.mode = GPIO_MODE_OUTPUT;
    hold_conf.pin_bit_mask = 1ULL << PG2_HOLD_GPIO;
    hold_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    hold_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&hold_conf));
    gpio_set_level(PG2_HOLD_GPIO, PG2_HOLD_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "PG2(GPIO%d)=%d immediate latch (reason=%d)",
             PG2_HOLD_GPIO, PG2_HOLD_ACTIVE_LEVEL, (int)esp_reset_reason());
}

void EspS3Cat::InitializePowerKeyGpio()
{
    gpio_config_t key_conf = {};
    key_conf.intr_type = GPIO_INTR_DISABLE;
    key_conf.mode = GPIO_MODE_INPUT;
    key_conf.pin_bit_mask = 1ULL << PG1_POWER_KEY_GPIO;
    key_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    key_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&key_conf));
    ESP_LOGI(TAG, "PG1 power key ready: GPIO%d active_level=%d",
             PG1_POWER_KEY_GPIO, PG1_POWER_KEY_ACTIVE_LEVEL);
}

int EspS3Cat::ReadPowerKeyLevel() const
{
    return gpio_get_level(PG1_POWER_KEY_GPIO);
}

bool EspS3Cat::IsPowerKeyPressed() const
{
    return ReadPowerKeyLevel() == PG1_POWER_KEY_ACTIVE_LEVEL;
}

void EspS3Cat::InitializePowerKey()
{
    InitializePowerLatch();
    InitializePowerKeyGpio();
    xTaskCreate(power_key_task, "power_key", 3072, this, 3, &power_key_task_handle_);
}


void EspS3Cat::InitializeCst816sTouchPad()
{
    cst816s_touch_ = new Cst816sTouch(i2c_bus_);
    if (!cst816s_touch_->init(DISPLAY_WIDTH,
                              DISPLAY_HEIGHT,
                              DISPLAY_SWAP_XY,
                              DISPLAY_MIRROR_X,
                              DISPLAY_MIRROR_Y)) {
        ESP_LOGE(TAG, "Failed to initialize CST816S touch controller");
        delete cst816s_touch_;
        cst816s_touch_ = nullptr;
        return;
    }

    // Register touch with LVGL adapter
    // Note: This should be called after display is initialized
    lv_display_t *disp = lv_display_get_default();
    if (disp == nullptr) {
        ESP_LOGW(TAG, "Display not initialized yet, touch will be registered later");
        return;
    }

    esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, cst816s_touch_->get_handle());
    lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
    if (touch == nullptr) {
        ESP_LOGE(TAG, "Touch registration failed");
        return;
    }

    ui_bridge_attach_gesture_handler(touch);
    ESP_LOGI(TAG, "Touch registered successfully");
}

void EspS3Cat::InitializeTouchSensor(uint8_t pcb_verison)
{
    touch_sensor_ = new TouchSensor();
    if (!touch_sensor_->init(pcb_verison)) {
        ESP_LOGE(TAG, "Failed to initialize touch sensor");
        delete touch_sensor_;
        touch_sensor_ = nullptr;
    } else {
        ESP_LOGI(TAG, "Touch sensor initialized successfully");
    }
}

void EspS3Cat::ShowHappyTouchFeedback()
{
    static int64_t s_last_us = 0;
    constexpr int64_t kCooldownUs = 1200000;
    const int64_t now = esp_timer_get_time();
    if ((now - s_last_us) < kCooldownUs) {
        return;
    }
    s_last_us = now;
    if (display_ != nullptr) {
        display_->SetEmotion("happy");
    }
}

void EspS3Cat::head_touch_gpio_task(void* arg)
{
    auto* self = static_cast<EspS3Cat*>(arg);
    constexpr int kPollMs = 20;
    constexpr int kDebounceSamples = 3;
    constexpr int64_t kTriggerCooldownUs = 800000;

    int stable_level = gpio_get_level(HEAD_TOUCH_GPIO);
    int last_raw_level = stable_level;
    int same_count = 0;
    bool was_active = (stable_level == HEAD_TOUCH_ACTIVE_LEVEL);
    int64_t last_trigger_us = 0;

    while (true) {
        const int raw_level = gpio_get_level(HEAD_TOUCH_GPIO);
        if (raw_level == last_raw_level) {
            ++same_count;
        } else {
            last_raw_level = raw_level;
            same_count = 1;
        }

        if (same_count >= kDebounceSamples && raw_level != stable_level) {
            stable_level = raw_level;
            const bool active = (stable_level == HEAD_TOUCH_ACTIVE_LEVEL);
            if (self != nullptr && active && !was_active) {
                const int64_t now = esp_timer_get_time();
                if ((now - last_trigger_us) >= kTriggerCooldownUs) {
                    last_trigger_us = now;
                    ESP_LOGI(TAG, "Head touch GPIO%d active", (int)HEAD_TOUCH_GPIO);
                    if (!ui_bridge_is_on_home_page() || ui_bridge_interactions_suppressed()) {
                        ESP_LOGI(TAG, "Head touch ignored (home=%d, suppressed=%d)",
                                 ui_bridge_is_on_home_page(), ui_bridge_interactions_suppressed());
                        was_active = active;
                        continue;
                    }
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateListening || app.GetDeviceState() == kDeviceStateSpeaking) {
                        app.TriggerWakeWord(Lang::Strings::TOUCH_HEAD);
                    } else if (app.GetDeviceState() == kDeviceStateIdle) {
                        app.WakeWordInvoke(std::string(Lang::Strings::TOUCH_HEAD));
                    }
                    self->ShowHappyTouchFeedback();
                }
            }
            was_active = active;
        }

        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

void EspS3Cat::InitializeHeadTouchGpio()
{
    if (HEAD_TOUCH_GPIO == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Head touch GPIO disabled: GPIO_NUM_NC");
        return;
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << HEAD_TOUCH_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t gpio_err = gpio_config(&io_conf);
    if (gpio_err != ESP_OK) {
        ESP_LOGW(TAG, "Head touch GPIO%d config failed: %s", (int)HEAD_TOUCH_GPIO, esp_err_to_name(gpio_err));
        return;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(head_touch_gpio_task, "head_touch", 3072, this, 3,
                                                  &head_touch_task_handle_, 1);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Head touch GPIO%d task create failed", (int)HEAD_TOUCH_GPIO);
        return;
    }
    ESP_LOGI(TAG, "Head touch GPIO%d ready, active_level=%d", (int)HEAD_TOUCH_GPIO, HEAD_TOUCH_ACTIVE_LEVEL);
}

namespace Qmi8658Motion {
static i2c_master_dev_handle_t qmi_dev_ = nullptr;

struct RawSample {
    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;
    int16_t gx = 0;
    int16_t gy = 0;
    int16_t gz = 0;
};

static esp_err_t WriteReg(uint8_t reg, uint8_t value)
{
    if (qmi_dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(qmi_dev_, data, sizeof(data), 100);
}

static esp_err_t ReadRegs(uint8_t reg, uint8_t* data, size_t len)
{
    if (qmi_dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(qmi_dev_, &reg, 1, data, len, 100);
}

static esp_err_t Initialize(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
{
    if (qmi_dev_ != nullptr) {
        return ESP_OK;
    }
    if (i2c_bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &qmi_dev_);
    if (ret != ESP_OK || qmi_dev_ == nullptr) {
        qmi_dev_ = nullptr;
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    uint8_t who = 0;
    ret = ReadRegs(0x00, &who, 1);
    if (ret != ESP_OK || who != 0x05) {
        i2c_master_bus_rm_device(qmi_dev_);
        qmi_dev_ = nullptr;
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }

    ret = WriteReg(0x02, 0x60);
    if (ret != ESP_OK) return ret;
    ret = WriteReg(0x03, (0x02 << 4) | 0x05);
    if (ret != ESP_OK) return ret;
    ret = WriteReg(0x04, (0x04 << 4) | 0x05);
    if (ret != ESP_OK) return ret;
    ret = WriteReg(0x08, 0x03);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "QMI8658A initialized at I2C address 0x%02X", addr);
    return ESP_OK;
}

static bool ReadRaw(RawSample& sample)
{
    uint8_t data[12] = {};
    if (ReadRegs(0x35, data, sizeof(data)) != ESP_OK) {
        return false;
    }
    auto le16 = [](const uint8_t* p) -> int16_t {
        return static_cast<int16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
    };
    sample.ax = le16(&data[0]);
    sample.ay = le16(&data[2]);
    sample.az = le16(&data[4]);
    sample.gx = le16(&data[6]);
    sample.gy = le16(&data[8]);
    sample.gz = le16(&data[10]);
    return true;
}
} // namespace Qmi8658Motion

void EspS3Cat::imu_event_task(void* arg)
{
    auto* board = static_cast<EspS3Cat*>(arg);
    if (board == nullptr) {
        vTaskDelete(NULL);
        return;
    }

    Qmi8658Motion::RawSample prev = {};
    Qmi8658Motion::RawSample cur = {};
    bool has_prev = false;
    int motion_hit_count = 0;
    int64_t last_shake_ms = 0;
    constexpr int kShakeDeltaThreshold = 36000;
    constexpr int kGyroDeltaThreshold = 45000;
    constexpr int kMotionHitRequired = 4;

    while (true) {
        if (Qmi8658Motion::ReadRaw(cur)) {
            if (has_prev) {
                int dx = abs(static_cast<int>(cur.ax) - static_cast<int>(prev.ax));
                int dy = abs(static_cast<int>(cur.ay) - static_cast<int>(prev.ay));
                int dz = abs(static_cast<int>(cur.az) - static_cast<int>(prev.az));
                int dgx = abs(static_cast<int>(cur.gx) - static_cast<int>(prev.gx));
                int dgy = abs(static_cast<int>(cur.gy) - static_cast<int>(prev.gy));
                int dgz = abs(static_cast<int>(cur.gz) - static_cast<int>(prev.gz));
                int shake_score = dx + dy + dz;
                int gyro_score = dgx + dgy + dgz;

                const int64_t now_ms = esp_timer_get_time() / 1000;
                if ((shake_score > kShakeDeltaThreshold) || (gyro_score > kGyroDeltaThreshold)) {
                    motion_hit_count++;
                    if (motion_hit_count >= kMotionHitRequired &&
                        (now_ms - last_shake_ms) > static_cast<int64_t>(board->SHAKE_COOLDOWN_MS)) {
                        last_shake_ms = now_ms;
                        motion_hit_count = 0;
                        if (ui_bridge_is_on_home_page() && board->shake_enabled_) {
                            board->handle_violent_shake_event();
                        }
                    }
                } else {
                    if (motion_hit_count > 0) motion_hit_count--;
                }
            }
            prev = cur;
            has_prev = true;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}
    // 处理剧烈甩动事件
void EspS3Cat::handle_violent_shake_event() {
    auto &app = Application::GetInstance();
    auto display = GetDisplay();
    
    ESP_LOGI(TAG, "🚨 设备被剧烈甩动，触发响应");
    if(ui_bridge_is_on_home_page()&&shake_enabled_)
    {
        auto music = Board::GetInstance().GetMusic();  
        bool music_play = music->MusicPlaying();
        ESP_LOGI(TAG, "当前放歌状态：%s", music_play ? "是" : "否");
        if (app.GetDeviceState() == kDeviceStateListening || 
            app.GetDeviceState() == kDeviceStateSpeaking) {
            if(music_play){
                music->StopStreaming();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            app.TriggerWakeWord(Lang::Strings::SHAKE);//"晃你猫头,你会怎样？"
            if (display) {
                display->SetEmotion("confused");
            }
        } 
        else if (app.GetDeviceState() == kDeviceStateIdle) {
            if(music_play){
                music->StopStreaming();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            app.WakeWordInvoke(Lang::Strings::SHAKE_AWAKE);//"晃你猫头,你会怎样？"
            if (display) {
                display->SetEmotion("confused");
            }
        }
    }
    
    
    // 振动反馈（如果有振动马达）
    // provide_haptic_feedback();
}
void EspS3Cat::InitializeQmi8658()
{
    static constexpr uint8_t kQmi8658Addrs[] = {0x6A, 0x6B};
    for (uint8_t addr : kQmi8658Addrs) {
        if (i2c_master_probe(i2c_bus_, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "QMI8658A detected at I2C address 0x%02X", addr);
            if (Qmi8658Motion::Initialize(i2c_bus_, addr) == ESP_OK) {
                qmi8658_ready_ = true;
                xTaskCreatePinnedToCore(imu_event_task, "qmi8658_evt", 3072, this, 3, &imu_task_handle_, 1);
                ESP_LOGI(TAG, "QMI8658A motion task started");
            } else {
                ESP_LOGW(TAG, "QMI8658A init failed, shake disabled");
            }
            return;
        }
    }
    ESP_LOGW(TAG, "QMI8658A not found on I2C bus, shake disabled");
}
//初始化bq272220芯片(电量监控芯片)
void EspS3Cat::Initializebq27220()
{
    if (i2c_bus == NULL) {
        ESP_LOGE("BQ27220", "I2C bus is NULL, initialization failed");
        return;
    }
    
    bq27220_config_t bq27220_cfg = {
        .i2c_bus = i2c_bus,
        .cfg = &default_config,
        .cedv = &default_cedv,
    };
    
    bq27220 = bq27220_create(&bq27220_cfg);
    
    if (bq27220) {
        ESP_LOGI("BQ27220", "BQ27220 initialized successfully");
    } else {
        if(!bq27220_create(&bq27220_cfg))
        {
            ESP_LOGE("BQ27220", "BQ27220 initialization failed");
        }
    }
    
}
void EspS3Cat::InitializeTFCard(){
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   // 如果挂载不成功是否需要格式化SD卡,需要FAT格式
        .max_files = 5, // 允许打开的最大文件数
        .allocation_unit_size = 16 * 1024  // 分配单元大小
    };
    
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "gpio_get_level SD card");
    ESP_LOGI(TAG, "Using SDMMC peripheral");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT(); // SDMMC主机接口配置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT(); // SDMMC插槽配置
    slot_config.width = 1;  // 设置为1线SD模式
    slot_config.clk = BSP_SD_CLK; 
    slot_config.cmd = BSP_SD_CMD;
    slot_config.d0 = BSP_SD_D0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; // 打开内部上拉电阻

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card); // 挂载SD卡

    if (ret != ESP_OK) {  // 如果没有挂载成功
        if (ret == ESP_FAIL) { // 如果挂载失败
            ESP_LOGE(TAG, "Failed to mount filesystem. ");
        } else { // 如果是其它错误 打印错误名称
            ESP_LOGE(TAG, "Failed to initialize the card (%s). ", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted"); // 提示挂载成功
    sdmmc_card_print_info(stdout, card); // 终端打印SD卡的一些信息
}

EspS3Cat::EspS3Cat()
    : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 0, ML307_EN_PIN, ML307_UART_NUM),
      boot_button_(BOOT_BUTTON_GPIO)
{
    InitializeI2c();//首先进行I2C初始化
    uint8_t pcb_verison = 0;
    ESP_LOGI(TAG, "CyberVoc V2_0 init");
    InitializePowerKey();
    
    InitializeSpi();
    ESP_LOGI(TAG, "SPI initialized");
    Initializest77916Display(pcb_verison);
    ESP_LOGI(TAG, "Display initialized");
    InitializeButtons();
    ESP_LOGI(TAG, "Buttons initialized");
    InitializeCst816sTouchPad();
    InitializeHeadTouchGpio();

    InitializeQmi8658();
    vTaskDelay(pdMS_TO_TICKS(30)); // 短暂延迟
    Initializebq27220();//后于姿态传感器初始化
    ESP_LOGI(TAG, "================================================================================================================");
    InitializeTFCard();

    // Initialize modules
    base_control_ = new BaseControl(this);
    base_control_->Initialize(pcb_verison);

    audio_analysis_ = new AudioAnalysis();
    audio_analysis_->Initialize();

    CyberVocTools::Initialize(this);
}

AudioCodec* EspS3Cat::GetAudioCodec()
{
    static BoxAudioCodec audio_codec(
        i2c_bus_,
        AUDIO_INPUT_SAMPLE_RATE,
        AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK,
        AUDIO_I2S_GPIO_BCLK,
        AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_DOUT,
        kAudioI2sDin,
        kAudioPaPin,
        AUDIO_CODEC_ES8311_ADDR,
        AUDIO_CODEC_ES7210_ADDR,
        AUDIO_INPUT_REFERENCE);
    return &audio_codec;
}

Display* EspS3Cat::GetDisplay()
{
    return display_;
}

esp_lcd_touch_handle_t EspS3Cat::GetTouchpad()
{
    return cst816s_touch_ != nullptr ? cst816s_touch_->get_handle() : nullptr;
}

Backlight* EspS3Cat::GetBacklight()
{
    return backlight_;
}

void EspS3Cat::SetAfeDataProcessCallback(std::function<void(const int16_t* audio_data, size_t total_bytes)> callback)
{
    (void)callback;  // Unused, processing is done in AudioAnalysis
    if (audio_analysis_ != nullptr) {
        audio_analysis_->SetAfeDataProcessCallback();
    }
}

void EspS3Cat::SetVadStateChangeCallback(std::function<void(bool speaking)> callback)
{
    (void)callback;  // Unused, processing is done in AudioAnalysis
    if (audio_analysis_ != nullptr) {
        audio_analysis_->SetVadStateChangeCallback();
    }
}

void EspS3Cat::SetAudioDataProcessedCallback(std::function<void(const int16_t* audio_data, size_t bytes_per_channel, size_t channels)> callback)
{
    (void)callback;  // Unused, processing is done in AudioAnalysis
    if (audio_analysis_ != nullptr) {
        audio_analysis_->SetAudioDataProcessedCallback();
    }
}

void EspS3Cat::SetAudioAnalysisMode(AudioAnalysisMode mode)
{
    if (audio_analysis_ != nullptr) {
        audio_analysis_->SetMode(mode);
    }
}

bool EspS3Cat::GetBatteryLevel(int &level, bool& charging, bool& discharging)
    {
        level = bq27220_get_state_of_charge(bq27220);
        battery_status_t status = {};
        bq27220_get_battery_status(bq27220, &status);
        
        int16_t current = bq27220_get_current(bq27220);
        uint16_t voltage = bq27220_get_voltage(bq27220);
        uint16_t soc = bq27220_get_state_of_charge(bq27220);
        level = (int)soc;
        // ESP_LOGI(TAG,"=== 电池状态 ===\n");
        // ESP_LOGI(TAG,"电量: %d%%\n", soc);
        //ESP_LOGI(TAG,"电压: %dmV\n", voltage);
        // ESP_LOGI(TAG,"电流: %dmA\n", current);
        // ESP_LOGI(TAG,"温度: %d°C\n", bq27220_get_temperature(bq27220) / 10 - 273);
        
        // 充电状态判断
        if (status.FC) {
            //ESP_LOGI(TAG,"状态: 🔋 已充满\n");
        } else if (current > 20) {
            //ESP_LOGI(TAG,"状态: ⚡ 充电中 (电流: %dmA)\n", current);
            charging = true;
            discharging = false;
        } else if (current < -20) {
            //ESP_LOGI(TAG,"状态: 🔌 放电中 (电流: %dmA)\n", current);
            charging = false;
            discharging = true;
        } else {
            //ESP_LOGI(TAG,"状态: ⏸️  空闲\n");
        }
        
        // 其他状态标志
        // ESP_LOGI(TAG,"标志: %s%s%s%s\n",
        //     status.DSG ? "[放电]" : "",
        //     status.CHGINH ? "[充电禁止]" : "",
        //     status.BATTPRES ? "[电池存在]" : "[无电池]",
        //     status.OTC || status.OTD ? "[过温]" : "");
        return true;
    }

DECLARE_BOARD(EspS3Cat);
