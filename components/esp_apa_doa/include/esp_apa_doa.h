/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_APA_DOA_NUM_BINS_DEFAULT  12  /*!< Default direction bins for a 12-LED ring */
#define ESP_APA_DOA_NUM_BINS_MIN       4  /*!< Minimum supported direction bins */
#define ESP_APA_DOA_NUM_BINS_MAX      72  /*!< Maximum supported direction bins */

/**
 * @brief  PCM input buffer layout consumed by esp_apa_doa_process()
 */
typedef enum {
    ESP_APA_DOA_INPUT_LAYOUT_PACKED = 0,  /*!< Packed as mic0[0], mic1[0], mic2[0], mic0[1], ... */
    ESP_APA_DOA_INPUT_LAYOUT_PLANAR,      /*!< Planar as mic0[], then mic1[], then mic2[] */
} esp_apa_doa_input_layout_t;

/**
 * @brief  ESP APA DOA runtime configuration.
 *
 *         Coordinates: +Y = 0 degrees (front), azimuth increases clockwise
 *         (90 = right, 180 = back, 270 = left). The configuration is copied by
 *         esp_apa_doa_create() and is read-only inside the engine.
 *
 *         Pipeline: external VAD gate -> delay estimation -> geometric solve ->
 *         gates -> smoothing -> direction bin.
 *
 *         num_mics selects the array (engine sets collinear = true for 2-mic):
 *           2 -> linear pair on X axis; 180 deg user azimuth (0 = left mic,
 *                90 = front, 180 = right mic).
 *           3 -> equilateral triangle; 360 deg (default).
 *           4 -> square; 360 deg (engine math ready).
 *
 *         Use ESP_APA_DOA_CFG_DEFAULT() as the baseline. struct_size is reserved
 *         for future ABI checks; a value of 0 is currently accepted for backward
 *         compatibility.
 */
typedef struct {
    uint32_t                    struct_size;         /*!< Size of this structure, set to sizeof(esp_apa_doa_cfg_t) */
    float                       sample_rate_hz;      /*!< Input PCM sample rate in Hz */
    int                         window_samples;      /*!< Analysis window length per mic; power of 2 and >= 32 */
    int                         chunk_samples;       /*!< Samples per mic per process call; > 0 and <= window_samples */
    int                         num_mics;            /*!< Mic count, currently 2..4 (required) */
    float                       mic_side_m;          /*!< Adjacent mic spacing in meters (baseline or polygon side) */
    esp_apa_doa_input_layout_t  input_layout;        /*!< PCM layout consumed by esp_apa_doa_process() */
    int                         num_bins;            /*!< Direction bins; 0 uses ESP_APA_DOA_NUM_BINS_DEFAULT */
    float                       azimuth_offset_deg;  /*!< Calibration offset subtracted from raw azimuth in degrees */
    int                         gcc_stride;          /*!< DOA analysis run interval: 1 = every chunk, N = every Nth; 0 uses default 2 */
    float                       fmax_hz;             /*!< Analysis band upper edge in Hz; 0 uses default 2500 */
    float                       level_min_db_2mic;   /*!< 2-mic only: minimum signal level gate in dBFS; 0 uses default -55 */
    uint32_t                    reserved[4];         /*!< Reserved for future use; set to 0 */
} esp_apa_doa_cfg_t;

/**
 * @brief  Default tuning for Korvo-1 3-mic capture at 16 kHz (Balanced preset)
 *
 *         mic_side_m is the mic-to-mic triangle side length (6.5 cm on Korvo-1).
 *         Override num_mics, mic_side_m, and chunk_samples for your board geometry.
 *         Use ESP_APA_DOA_PRESET_ECONOMY / ESP_APA_DOA_PRESET_RESPONSIVE to
 *         switch gcc_stride, chunk_samples, and fmax_hz as a tuned set.
 */
#define ESP_APA_DOA_CFG_DEFAULT() ((esp_apa_doa_cfg_t){ \
    .struct_size        = sizeof(esp_apa_doa_cfg_t), \
    .sample_rate_hz     = 16000.0f, \
    .window_samples     = 512, \
    .chunk_samples      = 256, \
    .num_mics           = 3, \
    .mic_side_m         = 0.065f, \
    .input_layout       = ESP_APA_DOA_INPUT_LAYOUT_PACKED, \
    .num_bins           = ESP_APA_DOA_NUM_BINS_DEFAULT, \
    .azimuth_offset_deg = 0.0f, \
    .gcc_stride         = 2, \
    .fmax_hz            = 2500.0f, \
    .level_min_db_2mic  = -55.0f, \
})

/**
 * @brief  Apply Economy preset: lower CPU, slower tracking
 *         stride=4, chunk=384, fmax=2000 Hz
 */
#define ESP_APA_DOA_PRESET_ECONOMY(cfg) do { \
    (cfg)->gcc_stride    = 4; \
    (cfg)->chunk_samples = 384; \
    (cfg)->fmax_hz       = 2000.0f; \
} while (0)

/**
 * @brief  Apply Balanced preset (same as CFG_DEFAULT)
 *         stride=2, chunk=256, fmax=2500 Hz
 */
#define ESP_APA_DOA_PRESET_BALANCED(cfg) do { \
    (cfg)->gcc_stride    = 2; \
    (cfg)->chunk_samples = 256; \
    (cfg)->fmax_hz       = 2500.0f; \
} while (0)

/**
 * @brief  Apply Responsive preset: fast tracking, highest CPU
 *         stride=1, chunk=256, fmax=2500 Hz
 */
#define ESP_APA_DOA_PRESET_RESPONSIVE(cfg) do { \
    (cfg)->gcc_stride    = 1; \
    (cfg)->chunk_samples = 256; \
    (cfg)->fmax_hz       = 2500.0f; \
} while (0)

/**
 * @brief  DOA result for one analysis frame
 *
 *         azimuth: 2-mic -> 0..180 (0 = left, 90 = front, 180 = right);
 *         3+ mics -> 0..360 (0 = front, 90 = right, clockwise).
 */
typedef struct {
    bool   valid;          /*!< True when this frame produced and accepted a new DOA estimate */
    float  azimuth;        /*!< Smoothed azimuth in degrees */
    int    direction_bin;  /*!< Direction sector index in [0, cfg.num_bins) */
} esp_apa_doa_result_t;

/**
 * @brief  Opaque DOA engine handle
 */
typedef struct esp_apa_doa_engine_t *esp_apa_doa_handle_t;

/**
 * @brief  Create a DOA engine
 *
 * @note   The caller owns the returned handle and must release it with
 *         esp_apa_doa_destroy(). The function copies cfg, so the caller may release
 *         or modify cfg after this function returns.
 *
 * @note   This API allocates heap memory for internal analysis buffers. It is not
 *         ISR-safe and APIs on the same handle are not thread-safe.
 *
 * @param[in]   cfg         Engine configuration; use ESP_APA_DOA_CFG_DEFAULT() as baseline
 * @param[out]  out_handle  DOA handle on success; undefined on error
 *
 * @return
 *       - ESP_OK               On success
 *       - ESP_ERR_INVALID_ARG  cfg or out_handle is NULL, or cfg is out of range
 *       - ESP_ERR_NO_MEM       Internal allocation failed
 */
esp_err_t esp_apa_doa_create(const esp_apa_doa_cfg_t *cfg, esp_apa_doa_handle_t *out_handle);

/**
 * @brief  Feed one PCM chunk and update the DOA estimate
 *
 *         The caller passes the newly read chunk, not a full analysis window.
 *         The engine maintains the cfg.window_samples sliding history internally.
 *         Until enough samples are accumulated, out->valid is false. If an external
 *         VAD is used, pass out == NULL for silent chunks to update only the
 *         internal history and skip DOA calculation.
 *
 * @note   out is valid only for this call. This API is not ISR-safe and must not be
 *         called concurrently on the same handle.
 *
 * @param[in]   h             DOA handle returned by esp_apa_doa_create()
 * @param[in]   pcm           New PCM chunk in cfg.input_layout
 * @param[in]   sample_count  Total int16 sample count; must be cfg.num_mics * cfg.chunk_samples
 * @param[out]  out           Result buffer populated on success; NULL updates history only
 *
 * @return
 *       - ESP_OK                Chunk accepted and, when history is full, processed
 *       - ESP_ERR_INVALID_ARG   h or pcm is NULL
 *       - ESP_ERR_INVALID_SIZE  sample_count does not match cfg.num_mics * cfg.chunk_samples
 *       - ESP_FAIL              Internal DOA analysis failed
 */
esp_err_t esp_apa_doa_process(esp_apa_doa_handle_t h, const int16_t *pcm,
                              size_t sample_count, esp_apa_doa_result_t *out);

/**
 * @brief  Reset transient DOA tracking state
 *
 *         Clears cached delay/confidence estimates, analysis stride counters, and
 *         jump confirmation state. The internal PCM history is kept so a caller can
 *         keep feeding silent/pre-roll chunks and get a low-latency estimate when
 *         speech starts. The smoothed direction and direction bin are also kept,
 *         so UI can hold the last accepted direction.
 *
 * @note   Passing NULL is allowed. This API is not ISR-safe and must not be called
 *         concurrently with esp_apa_doa_process() on the same handle.
 *
 * @param[in]  h  DOA handle returned by esp_apa_doa_create()
 */
void esp_apa_doa_reset(esp_apa_doa_handle_t h);

/**
 * @brief  Destroy a DOA engine
 *
 * @note   Passing NULL is allowed. After this call the handle is invalid and must
 *         not be used again. This API is not ISR-safe.
 *
 * @param[in]  h  DOA handle returned by esp_apa_doa_create()
 */
void esp_apa_doa_destroy(esp_apa_doa_handle_t h);

/**
 * @brief  Get the DOA preset label inferred from the active configuration
 *
 * @param[in]  h  DOA handle returned by esp_apa_doa_create()
 *
 * @return Static string: "economy", "balanced", "responsive", or "custom"
 */
const char *esp_apa_doa_profile_name(esp_apa_doa_handle_t h);

#ifdef __cplusplus
}
#endif
