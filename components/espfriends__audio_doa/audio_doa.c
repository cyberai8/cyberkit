/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/event_groups.h"

#include "audio_doa.h"

#include "esp_doa.h"
#include "esp_log.h"

#define TAG "AUDIO_DOA"

#define AUDIO_DOA_DATA_BUS_SIZE 2048

#define DOA_WINDOW_SIZE 7
#define GAUSSIAN_SIGMA  1.0

#define START_BIT (1 << 0)

typedef enum {
    AUDIO_DOA_STATE_IDLE,
    AUDIO_DOA_STATE_RUNNING,
    AUDIO_DOA_STATE_ERROR,
} audio_doa_state_t;

typedef enum {
    MIC_DIRECTION_LEFT,
    MIC_DIRECTION_RIGHT,
    MIC_DIRECTION_MAX,
} mic_direction_t;

typedef struct {
    audio_doa_state_t     state;
    audio_doa_callback_t  cb;
    void                 *ctx;
    uint8_t              *audio_data;
    int                   audio_data_size;
    doa_handle_t         *doa_handle;
    StreamBufferHandle_t  stream_buffer;
    TaskHandle_t          task_handle;
    EventGroupHandle_t    event_group;
    int16_t              *mic_data[MIC_DIRECTION_MAX];
    float                 doa_history[DOA_WINDOW_SIZE];
    int                   doa_history_index;
    float                *gaussian_weights;
    float                 mic_distance;
} audio_doa_t;

static float moving_weighted_average(float *data, int window_size, float *weights, int current_index)
{
    float sum = 0.0f;
    float weight_sum = 0.0f;

    for (int i = 0; i < window_size; i++) {
        int data_index = (current_index - i + window_size) % window_size;
        sum += data[data_index] * weights[i];
        weight_sum += weights[i];
    }

    return sum / weight_sum;
}

static void generate_gaussian_weights(float *weights, int size, float sigma)
{
    float sum = 0.0f;
    float center = (size - 1) / 2.0f;

    for (int i = 0; i < size; i++) {
        float x = i - center;
        weights[i] = expf(-(x * x) / (2 * sigma * sigma));
        sum += weights[i];
    }

    for (int i = 0; i < size; i++) {
        weights[i] /= sum;
    }
}

typedef struct {
    float raw;
    float corrected;
} doa_calibration_point_t;

static const doa_calibration_point_t doa_calibration_table[] = {
    {0.0f,    0.0f},
    {60.0f,   20.0f},
    {80.0f,   45.0f},
    {90.0f,   90.0f},
    {100.0f, 135.0f},
    {120.0f, 160.0f},
    {180.0f, 180.0f},
};

static float interpolate_calibrated_angle(float raw_angle)
{
    const size_t count = sizeof(doa_calibration_table) / sizeof(doa_calibration_table[0]);

    if (raw_angle <= doa_calibration_table[0].raw) {
        return doa_calibration_table[0].corrected;
    }
    if (raw_angle >= doa_calibration_table[count - 1].raw) {
        return doa_calibration_table[count - 1].corrected;
    }

    for (size_t i = 1; i < count; ++i) {
        if (raw_angle <= doa_calibration_table[i].raw) {
            float raw_span = doa_calibration_table[i].raw - doa_calibration_table[i - 1].raw;
            float corrected_span = doa_calibration_table[i].corrected - doa_calibration_table[i - 1].corrected;
            float ratio = (raw_angle - doa_calibration_table[i - 1].raw) / raw_span;
            return doa_calibration_table[i - 1].corrected + corrected_span * ratio;
        }
    }

    return raw_angle;
}

static float doa_angle_calibration_with_distance(float raw_angle, float distance)
{
    float corrected_angle = interpolate_calibrated_angle(raw_angle);
    // ESP_LOGI(TAG, "Raw: %.2f, Corrected: %.2f", raw_angle, corrected_angle);

    float distance_factor = 1.0f + (distance - 15.0f) * 0.0025f;
    if (distance_factor < 0.92f) {
        distance_factor = 0.92f;
    } else if (distance_factor > 1.1f) {
        distance_factor = 1.1f;
    }

    corrected_angle /= distance_factor;

    if (corrected_angle < 0.0f) {
        corrected_angle = 0.0f;
    } else if (corrected_angle > 180.0f) {
        corrected_angle = 180.0f;
    }

    corrected_angle = corrected_angle * 0.95f + raw_angle * 0.05f;

    return corrected_angle;
}

static inline void extract_mic_data(audio_doa_t *doa)
{
    int16_t *audio_buffer = (int16_t *)doa->audio_data;
    int size = doa->audio_data_size / sizeof(int16_t);
    int sample_count = size / 2;  // 每个通道的样本数
    for (int i = 0; i < sample_count; i++) {
        doa->mic_data[MIC_DIRECTION_LEFT][i] = audio_buffer[i * 2];
        doa->mic_data[MIC_DIRECTION_RIGHT][i] = audio_buffer[i * 2 + 1];
    }
}

static void audio_doa_thread(void *arg)
{
    audio_doa_t *doa = (audio_doa_t *)arg;
    while (1) {
        uint32_t bits = xEventGroupWaitBits(doa->event_group, START_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10));
        if (!(bits & START_BIT)) {
            ESP_LOGI(TAG, "Audio DOA thread is not started");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        doa->state = AUDIO_DOA_STATE_RUNNING;
        
        size_t bytes_received = xStreamBufferReceive(doa->stream_buffer, 
                                                     doa->audio_data, 
                                                     AUDIO_DOA_DATA_BUS_SIZE, 
                                                     pdMS_TO_TICKS(10));
        if (bytes_received < AUDIO_DOA_DATA_BUS_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int16_t *audio_data = (int16_t *)doa->audio_data;
        float rms_value = 0.0f;
        for (int i = 0; i < AUDIO_DOA_DATA_BUS_SIZE / sizeof(int16_t); i++) {
            rms_value += (float)(audio_data[i]) * (float)(audio_data[i]);
        }
        rms_value = sqrtf(rms_value / (AUDIO_DOA_DATA_BUS_SIZE / sizeof(int16_t)));
        // ESP_LOGI(TAG, "RMS value: %.2f", rms_value);
        // if (rms_value < 80.0f) {
        //     continue;
        // }

        // for (int i = 0; i < 10; i++) {
        //     printf("channel1: %d, channel2: %d\n", audio_data[i * 2], audio_data[i * 2 + 1]);
        // }


        extract_mic_data(doa);
        float estimated_direction = esp_doa_process(doa->doa_handle, doa->mic_data[MIC_DIRECTION_LEFT], doa->mic_data[MIC_DIRECTION_RIGHT]);
        // if (rms_value >= 80.0f && doa->doa_history_index > 0) {
            // doa->doa_history[doa->doa_history_index] = doa->doa_history[doa->doa_history_index - 1];
        // } else{
            doa->doa_history[doa->doa_history_index] = estimated_direction;
        // }
        // doa->doa_history[doa->doa_history_index] = estimated_direction;
        float filtered_direction = moving_weighted_average(doa->doa_history, DOA_WINDOW_SIZE, doa->gaussian_weights, doa->doa_history_index);
        doa->doa_history_index = (doa->doa_history_index + 1) % DOA_WINDOW_SIZE;
        float calibrated_direction = doa_angle_calibration_with_distance(filtered_direction, doa->mic_distance);
        if (doa->cb) {
            doa->cb(calibrated_direction, doa->ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t audio_doa_new(audio_doa_handle_t *doa_handle, audio_doa_config_t *config)
{
    if (doa_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_t *doa = (audio_doa_t *)malloc(sizeof(audio_doa_t));
    if (doa == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(doa, 0, sizeof(audio_doa_t));
    doa->state = AUDIO_DOA_STATE_IDLE;
    doa->cb = NULL;
    doa->ctx = NULL;
    doa->doa_handle = NULL;
    doa->doa_history_index = 0;

    doa->event_group = xEventGroupCreate();
    if (doa->event_group == NULL) {
        free(doa);
        return ESP_ERR_NO_MEM;
    }
    doa->stream_buffer = xStreamBufferCreate(AUDIO_DOA_DATA_BUS_SIZE * 3, AUDIO_DOA_DATA_BUS_SIZE);
    if (doa->stream_buffer == NULL) {
        vEventGroupDelete(doa->event_group);
        free(doa);
        ESP_LOGE(TAG, "Failed to create stream buffer");
        return ESP_ERR_NO_MEM;
    }
    int samples = AUDIO_DOA_DATA_BUS_SIZE / (sizeof(int16_t) * 2);
    float mic_distance = 0.045085f;
    if (config && config->distance > 0.0f) {
        mic_distance = config->distance;
    }
    doa->mic_distance = mic_distance;
    doa->doa_handle = esp_doa_create(16000, 10, mic_distance, samples);
    if (doa->doa_handle == NULL) {
        vStreamBufferDelete(doa->stream_buffer);
        vEventGroupDelete(doa->event_group);
        free(doa);
        return ESP_ERR_NO_MEM;
    }
    doa->audio_data = (uint8_t *)calloc(AUDIO_DOA_DATA_BUS_SIZE, sizeof(uint8_t));
    if (doa->audio_data == NULL) {
        esp_doa_destroy(doa->doa_handle);
        vStreamBufferDelete(doa->stream_buffer);
        vEventGroupDelete(doa->event_group);
        free(doa);
        return ESP_ERR_NO_MEM;
    }
    doa->audio_data_size = AUDIO_DOA_DATA_BUS_SIZE;

    doa->gaussian_weights = (float *)calloc(DOA_WINDOW_SIZE, sizeof(float));
    if (doa->gaussian_weights == NULL) {
        free(doa->audio_data);
        esp_doa_destroy(doa->doa_handle);
        vStreamBufferDelete(doa->stream_buffer);
        vEventGroupDelete(doa->event_group);
        free(doa);
        return ESP_ERR_NO_MEM;
    }
    generate_gaussian_weights(doa->gaussian_weights, DOA_WINDOW_SIZE, GAUSSIAN_SIGMA);

    for (int i = 0; i < MIC_DIRECTION_MAX; i++) {
        doa->mic_data[i] = (int16_t *)calloc(AUDIO_DOA_DATA_BUS_SIZE / (sizeof(int16_t) * 2), sizeof(int16_t));
        if (doa->mic_data[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(doa->mic_data[j]);
            }
            free(doa->gaussian_weights);
            free(doa->audio_data);
            esp_doa_destroy(doa->doa_handle);
            vStreamBufferDelete(doa->stream_buffer);
            vEventGroupDelete(doa->event_group);
            free(doa);
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ret = xTaskCreate(audio_doa_thread, "audio_doa_thread", 4096, doa, 10, &doa->task_handle);
    if (ret != pdPASS) {
        for (int i = 0; i < MIC_DIRECTION_MAX; i++) {
            free(doa->mic_data[i]);
        }
        free(doa->gaussian_weights);
        free(doa->audio_data);
        esp_doa_destroy(doa->doa_handle);
        vStreamBufferDelete(doa->stream_buffer);
        vEventGroupDelete(doa->event_group);
        free(doa);
        ESP_LOGE(TAG, "Failed to create audio DOA thread");
        return ESP_FAIL;
    }

    *doa_handle = (audio_doa_handle_t)doa;
    return ESP_OK;
}
esp_err_t audio_doa_delete(audio_doa_handle_t doa_handle)
{
    if (doa_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_t *doa = (audio_doa_t *)doa_handle;

    audio_doa_stop(doa_handle);

    vTaskDelay(pdMS_TO_TICKS(100));

    if (doa->task_handle != NULL) {
        vTaskDelete(doa->task_handle);
    }

    if (doa->mic_data[MIC_DIRECTION_LEFT]) {
        free(doa->mic_data[MIC_DIRECTION_LEFT]);
    }
    if (doa->mic_data[MIC_DIRECTION_RIGHT]) {
        free(doa->mic_data[MIC_DIRECTION_RIGHT]);
    }
    if (doa->gaussian_weights) {
        free(doa->gaussian_weights);
    }
    if (doa->audio_data) {
        free(doa->audio_data);
    }
    if (doa->doa_handle) {
        esp_doa_destroy(doa->doa_handle);
    }
    if (doa->stream_buffer) {
        vStreamBufferDelete(doa->stream_buffer);
    }
    if (doa->event_group) {
        vEventGroupDelete(doa->event_group);
    }
    free(doa);
    return ESP_OK;
}

esp_err_t audio_doa_set_doa_result_callback(audio_doa_handle_t doa_handle, audio_doa_callback_t cb, void *ctx)
{
    if (doa_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_doa_t *doa = (audio_doa_t *)doa_handle;
    doa->cb = cb;
    doa->ctx = ctx;
    return ESP_OK;
}

esp_err_t audio_doa_start(audio_doa_handle_t doa_handle)
{
    if (doa_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_doa_t *doa = (audio_doa_t *)doa_handle;
    if (doa->doa_handle == NULL) {
        return ESP_FAIL;
    }
    doa->state = AUDIO_DOA_STATE_RUNNING;
    xEventGroupSetBits(doa->event_group, START_BIT);
    return ESP_OK;
}

esp_err_t audio_doa_stop(audio_doa_handle_t doa_handle)
{
    if (doa_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_doa_t *doa = (audio_doa_t *)doa_handle;
    doa->state = AUDIO_DOA_STATE_IDLE;
    xEventGroupClearBits(doa->event_group, START_BIT);
    return ESP_OK;
}

esp_err_t audio_doa_data_write(audio_doa_handle_t doa_handle, uint8_t *data, int data_size)
{
    if (doa_handle == NULL || data == NULL || data_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_doa_t *doa = (audio_doa_t *)doa_handle;

    if (doa->stream_buffer == NULL) {
        return ESP_FAIL;
    }
    
    size_t bytes_sent = xStreamBufferSend(doa->stream_buffer, data, data_size, pdMS_TO_TICKS(10));
    if (bytes_sent != data_size) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}



