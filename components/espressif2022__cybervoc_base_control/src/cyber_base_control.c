/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "cyber_base_control.h"

// ============================================================================
// Macros
// ============================================================================

// Frame header
#define FRAME_HEADER_1         0xAA
#define FRAME_HEADER_2         0x55

// Frame size definitions
#define FRAME_HEADER_SIZE      2
#define FRAME_LENGTH_SIZE      2
#define FRAME_CMD_SIZE         1
#define FRAME_CHECKSUM_SIZE     1
#define FRAME_MIN_SIZE         (FRAME_HEADER_SIZE + FRAME_LENGTH_SIZE + FRAME_CMD_SIZE + FRAME_CHECKSUM_SIZE)
#define FRAME_MAX_SIZE         256

// Task configuration
#define UART_TASK_STACK_SIZE   4096

// ============================================================================
// Type Definitions
// ============================================================================

typedef struct {
    uart_port_t uart_num;
    QueueHandle_t uart_queue;
    TaskHandle_t uart_task_handle;
    uint32_t rx_buffer_size;
    cyber_base_control_cmd_callback_t cmd_cb;
    void *user_ctx;
} cyber_base_control_ctx_t;

// ============================================================================
// Function Declarations
// ============================================================================

static esp_err_t cyber_base_control_send_command_frame(uint8_t cmd, uint16_t data);
static void cyber_base_control_uart_receive_task(void *pvParameters);
static bool cyber_base_control_parse_response_frame(uint8_t *frame, int frame_len, uint8_t *cmd, uint8_t **data, int *data_len);
static cyber_base_control_ctx_t *cyber_base_control_get_ctx(void);
static void cyber_base_control_reset_frame_sync(bool *frame_sync, int *frame_index, int *expected_frame_len);
static bool cyber_base_control_process_frame_byte(uint8_t byte, uint8_t *frame_buffer, bool *frame_sync, 
                                                  int *frame_index, int *expected_frame_len);
static void cyber_base_control_handle_complete_frame(cyber_base_control_ctx_t *ctx, uint8_t *frame_buffer, 
                                                     int expected_frame_len, bool *frame_sync, 
                                                     int *frame_index);

// ============================================================================
// Constants
// ============================================================================

static const char *TAG = "CyberBase";

// ============================================================================
// Global Variables
// ============================================================================

static cyber_base_control_ctx_t *s_cyber_base_control_ctx = NULL;

static cyber_base_control_ctx_t *cyber_base_control_get_ctx(void)
{
    return s_cyber_base_control_ctx;
}

esp_err_t cyber_base_control_init(const cyber_base_control_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cyber_base_control_ctx != NULL) {
        ESP_LOGW(TAG, "Cyber base control already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    cyber_base_control_ctx_t *ctx = (cyber_base_control_ctx_t *)calloc(1, sizeof(cyber_base_control_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }

    ctx->uart_num = config->uart_num;
    ctx->rx_buffer_size = config->rx_buffer_size > 0 ? config->rx_buffer_size : 1024;
    ctx->cmd_cb = config->cmd_cb;
    ctx->user_ctx = config->user_ctx;

    uart_config_t uart_config = {
        .baud_rate = config->baud_rate > 0 ? config->baud_rate : 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(ctx->uart_num, ctx->rx_buffer_size * 2, ctx->rx_buffer_size * 2, 20, &ctx->uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        free(ctx);
        return ret;
    }

    ret = uart_param_config(ctx->uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
        uart_driver_delete(ctx->uart_num);
        free(ctx);
        return ret;
    }

    ret = uart_set_pin(ctx->uart_num, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        uart_driver_delete(ctx->uart_num);
        free(ctx);
        return ret;
    }

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "uart_rx_%d", ctx->uart_num);
    BaseType_t task_ret = xTaskCreate(cyber_base_control_uart_receive_task, task_name, UART_TASK_STACK_SIZE, ctx, 10, &ctx->uart_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART receive task");
        uart_driver_delete(ctx->uart_num);
        free(ctx);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Cyber base control initialized: UART%d, TX=%d, RX=%d, Baud=%d", 
             ctx->uart_num, config->tx_pin, config->rx_pin, uart_config.baud_rate);

    s_cyber_base_control_ctx = ctx;

    return ESP_OK;
}

static esp_err_t cyber_base_control_send_command_frame(uint8_t cmd, uint16_t data)
{
    cyber_base_control_ctx_t *ctx = cyber_base_control_get_ctx();
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Cyber base control not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data_high = (data >> 8) & 0xFF;
    uint8_t data_low  = data & 0xFF;
    uint8_t length = 3;

    uint8_t frame[8];
    frame[0] = FRAME_HEADER_1;
    frame[1] = FRAME_HEADER_2;
    frame[2] = 0x00;
    frame[3] = length;
    frame[4] = cmd;
    frame[5] = data_high;
    frame[6] = data_low;
    frame[7] = cmd + data_high + data_low;

    printf("Output: %02X %02X %02X %02X %02X %02X %02X %02X\n", 
           frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
    uart_write_bytes(ctx->uart_num, (const char *)frame, sizeof(frame));

    return ESP_OK;
}

esp_err_t cyber_base_control_set_angle(int angle)
{
    ESP_LOGI(TAG, "Set cyber base angle: %d", angle);
    return cyber_base_control_send_command_frame(CYBER_BASE_CMD_SET_ANGLE, angle);
}

esp_err_t cyber_base_control_set_action(int action)
{
    ESP_LOGI(TAG, "Set cyber base action: %d", action);
    return cyber_base_control_send_command_frame(CYBER_BASE_CMD_SET_ACTION, action);
}

esp_err_t cyber_base_control_set_calibrate(void)
{
    ESP_LOGI(TAG, "Set cyber base calibrate");
    return cyber_base_control_send_command_frame(CYBER_BASE_CMD_SET_CALIBRATE, 0x10);
}

static bool cyber_base_control_parse_response_frame(uint8_t *frame, int frame_len, uint8_t *cmd, uint8_t **data, int *data_len)
{
    if (frame_len < FRAME_MIN_SIZE) {
        ESP_LOGW(TAG, "Frame too short: %d < %d", frame_len, FRAME_MIN_SIZE);
        return false;
    }

    if (frame[0] != FRAME_HEADER_1 || frame[1] != FRAME_HEADER_2) {
        ESP_LOGW(TAG, "Invalid frame header: %02X %02X", frame[0], frame[1]);
        return false;
    }

    uint16_t data_length = (frame[2] << 8) | frame[3];
    if (data_length == 0 || data_length > (FRAME_MAX_SIZE - FRAME_MIN_SIZE)) {
        ESP_LOGW(TAG, "Invalid data length: %d", data_length);
        return false;
    }

    int expected_frame_len = FRAME_HEADER_SIZE + FRAME_LENGTH_SIZE + data_length + FRAME_CHECKSUM_SIZE;
    if (frame_len < expected_frame_len) {
        ESP_LOGW(TAG, "Frame incomplete: got %d, expected %d", frame_len, expected_frame_len);
        return false;
    }

    *cmd = frame[4];

    *data = &frame[5];
    *data_len = data_length - FRAME_CMD_SIZE;

    uint8_t checksum = frame[4];
    for (int i = 1; i < data_length; i++) {
        checksum += frame[4 + i];
    }
    
    uint8_t received_checksum = frame[4 + data_length];
    if (checksum != received_checksum) {
        ESP_LOGW(TAG, "Checksum error: expected %02X, got %02X", checksum, received_checksum);
        return false;
    }

    return true;
}

static void cyber_base_control_reset_frame_sync(bool *frame_sync, int *frame_index, int *expected_frame_len)
{
    *frame_sync = false;
    *frame_index = 0;
    if (expected_frame_len) {
        *expected_frame_len = 0;
    }
}

static bool cyber_base_control_process_frame_byte(uint8_t byte, uint8_t *frame_buffer, bool *frame_sync, 
                                                  int *frame_index, int *expected_frame_len)
{
    if (!*frame_sync) {
        if (byte == FRAME_HEADER_1) {
            frame_buffer[0] = byte;
            *frame_index = 1;
            *frame_sync = true;
            *expected_frame_len = 0;
        }
        return false;
    }

    frame_buffer[(*frame_index)++] = byte;

    if (*frame_index == 2) {
        if (byte != FRAME_HEADER_2) {
            cyber_base_control_reset_frame_sync(frame_sync, frame_index, expected_frame_len);
            if (byte == FRAME_HEADER_1) {
                frame_buffer[0] = byte;
                *frame_index = 1;
                *frame_sync = true;
            }
        }
        return false;
    }

    if (*frame_index == 4) {
        uint16_t data_length = (frame_buffer[2] << 8) | frame_buffer[3];
        if (data_length == 0 || data_length > (FRAME_MAX_SIZE - FRAME_MIN_SIZE)) {
            ESP_LOGW(TAG, "Invalid data length: %d", data_length);
            cyber_base_control_reset_frame_sync(frame_sync, frame_index, expected_frame_len);
            if (byte == FRAME_HEADER_1) {
                frame_buffer[0] = byte;
                *frame_index = 1;
                *frame_sync = true;
            }
            return false;
        }

        *expected_frame_len = FRAME_HEADER_SIZE + FRAME_LENGTH_SIZE + data_length + FRAME_CHECKSUM_SIZE;
        if (*expected_frame_len > FRAME_MAX_SIZE) {
            ESP_LOGW(TAG, "Frame too large: %d", *expected_frame_len);
            cyber_base_control_reset_frame_sync(frame_sync, frame_index, expected_frame_len);
            return false;
        }
        return false;
    }

    if (*expected_frame_len > 0 && *frame_index >= *expected_frame_len) {
        return true;
    }

    return false;
}

static void cyber_base_control_handle_complete_frame(cyber_base_control_ctx_t *ctx, uint8_t *frame_buffer, 
                                                     int expected_frame_len, bool *frame_sync, 
                                                     int *frame_index)
{
    uint8_t cmd;
    uint8_t *data = NULL;
    int data_len = 0;

    if (cyber_base_control_parse_response_frame(frame_buffer, expected_frame_len, &cmd, &data, &data_len)) {
        if (ctx->cmd_cb) {
            ctx->cmd_cb(cmd, data, data_len, ctx->user_ctx);
        } else {
            ESP_LOGD(TAG, "Received command 0x%02X, data_len=%d (no callback)", cmd, data_len);
        }
    } else {
        ESP_LOGW(TAG, "Failed to parse frame");
    }

    cyber_base_control_reset_frame_sync(frame_sync, frame_index, NULL);
}

static void cyber_base_control_uart_receive_task(void *pvParameters)
{
    cyber_base_control_ctx_t *ctx = (cyber_base_control_ctx_t *)pvParameters;
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Invalid context in UART receive task");
        vTaskDelete(NULL);
        return;
    }

    uart_event_t event;
    uint8_t *rx_buffer = (uint8_t *)malloc(ctx->rx_buffer_size);
    uint8_t *frame_buffer = (uint8_t *)malloc(FRAME_MAX_SIZE);
    int frame_index = 0;
    int expected_frame_len = 0;
    bool frame_sync = false;

    if (rx_buffer == NULL || frame_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        if (rx_buffer) free(rx_buffer);
        if (frame_buffer) free(frame_buffer);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(ctx->uart_queue, (void *)&event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    {
                        int len = uart_read_bytes(ctx->uart_num, rx_buffer, event.size, portMAX_DELAY);
                        if (len > 0) {
                            for (int i = 0; i < len; i++) {
                                if (cyber_base_control_process_frame_byte(rx_buffer[i], frame_buffer, 
                                                                          &frame_sync, &frame_index, 
                                                                          &expected_frame_len)) {
                                    cyber_base_control_handle_complete_frame(ctx, frame_buffer, 
                                                                             expected_frame_len, 
                                                                             &frame_sync, &frame_index);
                                }
                            }
                        }
                        break;
                    }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(ctx->uart_num);
                    xQueueReset(ctx->uart_queue);
                    cyber_base_control_reset_frame_sync(&frame_sync, &frame_index, &expected_frame_len);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART buffer full");
                    uart_flush_input(ctx->uart_num);
                    xQueueReset(ctx->uart_queue);
                    cyber_base_control_reset_frame_sync(&frame_sync, &frame_index, &expected_frame_len);
                    break;
                case UART_BREAK:
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    cyber_base_control_reset_frame_sync(&frame_sync, &frame_index, &expected_frame_len);
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    cyber_base_control_reset_frame_sync(&frame_sync, &frame_index, &expected_frame_len);
                    break;
                default:
                    ESP_LOGD(TAG, "UART event type: %d", event.type);
                    break;
            }
        }
    }

    free(rx_buffer);
    free(frame_buffer);
    vTaskDelete(NULL);
}

esp_err_t cyber_base_control_deinit(void)
{
    cyber_base_control_ctx_t *ctx = cyber_base_control_get_ctx();
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Cyber base control not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->uart_task_handle != NULL) {
        vTaskDelete(ctx->uart_task_handle);
        ctx->uart_task_handle = NULL;
    }

    esp_err_t ret = uart_driver_delete(ctx->uart_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete UART driver");
    }

    free(ctx);
    s_cyber_base_control_ctx = NULL;

    ESP_LOGI(TAG, "Cyber base control deinitialized");
    return ESP_OK;
}

