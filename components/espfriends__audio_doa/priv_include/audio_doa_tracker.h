/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  Callback function type for DOA tracker result
 *
 * @param[in]  avg_angle  Average angle value calculated from the first 10 samples
 * @param[in]  ctx        User context pointer
 */
typedef void (*audio_doa_tracker_result_callback_t)(float avg_angle, void *ctx);

/**
 * @brief  Configuration structure for DOA tracker
 */
typedef struct {
    audio_doa_tracker_result_callback_t  result_callback;  /*!< Result callback function */
    void                                *ctx;              /*!< User context pointer */
    uint32_t                            output_interval_ms; /*!< Output interval in milliseconds (0 = output every time buffer is full) */
    float                               min_angle_change_threshold; /*!< Minimum angle change threshold in degrees (default: 15.0f, 0 = disabled) */
} audio_doa_tracker_cfg_t;

/**
 * @brief  Handle type for DOA tracker
 */
typedef void *audio_doa_tracker_handle_t;

/**
 * @brief  Initialize the DOA tracker
 *
 * @param[in]   cfg         Pointer to the configuration structure
 * @param[out]  out_handle  Pointer to the handle to be created
 *
 * @return
 *       - ESP_OK               Success
 *       - ESP_ERR_INVALID_ARG  Invalid argument
 *       - ESP_ERR_NO_MEM       Memory allocation failed
 */
esp_err_t audio_doa_tracker_init(audio_doa_tracker_cfg_t *cfg, audio_doa_tracker_handle_t *out_handle);

/**
 * @brief  Feed DOA angle value to the tracker
 *
 * @param[in]  handle  DOA tracker handle
 * @param[in]  angle   DOA angle value to feed
 *
 * @return
 *       - ESP_OK               Success
 *       - ESP_ERR_INVALID_ARG  Invalid argument
 */
esp_err_t audio_doa_tracker_feed(audio_doa_tracker_handle_t handle, float angle);

/**
 * @brief  Enable or disable the DOA tracker
 *
 *         When enabled (true), the tracker starts collecting the first 10 samples.
 *         When disabled (false), the buffer is cleared.
 *
 * @param[in]  handle  DOA tracker handle
 * @param[in]  enable  Enable (true) or disable (false) the tracker
 *
 * @return
 *       - ESP_OK               Success
 *       - ESP_ERR_INVALID_ARG  Invalid argument
 */
esp_err_t audio_doa_tracker_enable(audio_doa_tracker_handle_t handle, bool enable);

/**
 * @brief  Deinitialize the DOA tracker
 *
 * @param[in]  handle  DOA tracker handle
 *
 * @return
 *       - ESP_OK               Success
 *       - ESP_ERR_INVALID_ARG  Invalid argument
 */
esp_err_t audio_doa_tracker_deinit(audio_doa_tracker_handle_t handle);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
