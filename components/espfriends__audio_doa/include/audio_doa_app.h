#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define AUDIO_DOA_APP_BUFFER_MAX_SIZE_EACH_CHANNEL (1024)

// Forward declarations for opaque handles
typedef void *audio_doa_handle_t;
typedef void *audio_doa_tracker_handle_t;

typedef void (*audio_doa_result_callback_t)(float avg_angle, void *ctx);
typedef void (*audio_doa_monitor_callback_t)(float angle, void *ctx);

typedef struct {
    float                                       distance;
    audio_doa_monitor_callback_t                audio_doa_monitor_callback;
    void*                                       audio_doa_monitor_callback_ctx;
    audio_doa_result_callback_t                 audio_doa_result_callback;
    void*                                       audio_doa_result_callback_ctx;
} audio_doa_app_config_t;

typedef void *audio_doa_app_handle_t;

/**
 * @brief  Create a new audio DOA app instance
 * 
 * @param app 
 * @param config 
 * @return esp_err_t 
 */
esp_err_t audio_doa_app_create(audio_doa_app_handle_t *app, audio_doa_app_config_t *config);

/**
 * @brief  Start the audio DOA app
 * 
 * @param app 
 * @return esp_err_t 
 */
esp_err_t audio_doa_app_start(audio_doa_app_handle_t app);

/**
 * @brief  Stop the audio DOA app
 * 
 * @param app 
 * @return esp_err_t 
 */
esp_err_t audio_doa_app_stop(audio_doa_app_handle_t app);

/**
 * @brief  Destroy the audio DOA app instance
 * 
 * @param app 
 * @return esp_err_t 
 */
esp_err_t audio_doa_app_destroy(audio_doa_app_handle_t app);

/**
 * @brief  Write audio data to the audio DOA app
 * 
 * @param app 
 * @param data 
 * @param bytes_size 
 * @return esp_err_t
 */
esp_err_t audio_doa_app_data_write(audio_doa_app_handle_t app, uint8_t *data, int bytes_size);

/**
 * @brief  Set the VAD detect flag
 * 
 * @param app 
 * @param vad_detect 
 * @return esp_err_t
 */
esp_err_t audio_doa_app_set_vad_detect(audio_doa_app_handle_t app, bool vad_detect);

#ifdef __cplusplus
}
#endif  /* __cplusplus */