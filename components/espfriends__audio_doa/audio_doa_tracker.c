/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_doa_tracker.h"

static const char *TAG = "DOA_TRACKER";

#define DOA_TRACKER_BUFFER_SIZE 6
#define RECENT_WEIGHT_FACTOR 3.0f
#define REASONABLE_CHANGE_THRESHOLD 40.0f
#define SILENT_ANGLE 90.0f
#define SILENT_ANGLE_THRESHOLD 6.0f  // 84-96 degrees range
#define INITIAL_SAMPLES_TO_CHECK 3
#define GRADUAL_CHANGE_THRESHOLD 20.0f
#define ANGLE_QUANTIZATION_STEP 20.0f
#define ANGLE_MIN 0.0f
#define ANGLE_MAX 180.0f
#define MAJOR_ANGLE_CHANGE_THRESHOLD 30.0f
#define CONTINUOUS_90_DURATION_MS 1000
#define BUFFER_90_RATIO_THRESHOLD (2.0f / 3.0f)  // 2/3 of buffer must be near 90

/**
 * @brief  DOA tracker context structure
 */
typedef struct {
    bool                                 enabled;
    float                                buffer[DOA_TRACKER_BUFFER_SIZE];
    float                                original_buffer[DOA_TRACKER_BUFFER_SIZE];
    bool                                 valid_mask[DOA_TRACKER_BUFFER_SIZE];
    int                                  write_index;
    int                                  valid_count;
    bool                                 is_front_facing_mode;
    bool                                 is_not_front_facing_detected;
    int                                  initial_samples_count;
    float                                last_valid_angle;
    bool                                 has_last_valid_angle;
    float                                last_output_angle;
    bool                                 has_output_angle;
    TickType_t                           first_near_90_tick;
    bool                                 has_near_90_start;
    uint32_t                             output_interval_ms;
    TickType_t                           last_output_tick;
    float                                min_angle_change_threshold; /*!< Minimum angle change to trigger output */
    audio_doa_tracker_result_callback_t  result_callback;
    void                                *ctx;
} audio_doa_tracker_ctx_t;

// Forward declarations
static bool is_near_90_degrees(float angle);
static int count_near_90_in_buffer(audio_doa_tracker_ctx_t *ctx);
static bool check_continuous_90_duration(audio_doa_tracker_ctx_t *ctx);
static bool check_gradual_change_to_90(float angle, audio_doa_tracker_ctx_t *ctx);
static void reset_90_tracking(audio_doa_tracker_ctx_t *ctx);
static void start_90_tracking(audio_doa_tracker_ctx_t *ctx);

/**
 * @brief  Check if angle is near 90 degrees (84-96 range)
 */
static bool is_near_90_degrees(float angle)
{
    return fabsf(angle - SILENT_ANGLE) < SILENT_ANGLE_THRESHOLD;
}

/**
 * @brief  Count angles in buffer that are near 90 degrees (using original values)
 */
static int count_near_90_in_buffer(audio_doa_tracker_ctx_t *ctx)
{
    int count = 0;
    for (int i = 0; i < DOA_TRACKER_BUFFER_SIZE; i++) {
        if (ctx->valid_mask[i] && is_near_90_degrees(ctx->original_buffer[i])) {
            count++;
        }
    }
    return count;
}

/**
 * @brief  Check if buffer has mostly 90-degree angles
 */
static bool buffer_mostly_90(audio_doa_tracker_ctx_t *ctx)
{
    if (ctx->valid_count == 0) {
        return false;
    }
    int near_90_count = count_near_90_in_buffer(ctx);
    return (near_90_count >= (int)(ctx->valid_count * BUFFER_90_RATIO_THRESHOLD));
}

/**
 * @brief  Reset 90-degree tracking timer
 */
static void reset_90_tracking(audio_doa_tracker_ctx_t *ctx)
{
    ctx->has_near_90_start = false;
    ctx->first_near_90_tick = 0;
}

/**
 * @brief  Start tracking 90-degree angles
 */
static void start_90_tracking(audio_doa_tracker_ctx_t *ctx)
{
    if (!ctx->has_near_90_start) {
        ctx->first_near_90_tick = xTaskGetTickCount();
        ctx->has_near_90_start = true;
    }
}

/**
 * @brief  Check if continuous 90-degree angles for required duration
 */
static bool check_continuous_90_duration(audio_doa_tracker_ctx_t *ctx)
{
    if (!ctx->has_near_90_start) {
        return false;
    }
    
    TickType_t current_tick = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(CONTINUOUS_90_DURATION_MS);
    
    if ((current_tick - ctx->first_near_90_tick) >= duration_ticks) {
        ctx->is_front_facing_mode = true;
        ESP_LOGI(TAG, "Front-facing speech detected (continuous 90 degrees for %d ms)", CONTINUOUS_90_DURATION_MS);
        return true;
    }
    return false;
}

/**
 * @brief  Check if angle is gradually changing towards 90 degrees
 */
static bool check_gradual_change_to_90(float angle, audio_doa_tracker_ctx_t *ctx)
{
    if (!ctx->has_last_valid_angle || ctx->valid_count < 3) {
        return false;
    }
    
    float angle_change = fabsf(angle - ctx->last_valid_angle);
    if (angle_change >= GRADUAL_CHANGE_THRESHOLD) {
        return false;  // Not gradual
    }
    
    // Check if moving towards 90 degrees
    float prev_diff = fabsf(ctx->last_valid_angle - SILENT_ANGLE);
    float curr_diff = fabsf(angle - SILENT_ANGLE);
    
    if (curr_diff >= prev_diff) {
        return false;  // Not moving towards 90
    }
    
    // Check buffer trend: count angles moving towards 90
    int moving_towards_90 = 0;
    float last_checked = ctx->last_valid_angle;
    
    for (int i = 0; i < DOA_TRACKER_BUFFER_SIZE && moving_towards_90 < 3; i++) {
        int idx = (ctx->write_index - 2 - i + DOA_TRACKER_BUFFER_SIZE) % DOA_TRACKER_BUFFER_SIZE;
        if (ctx->valid_mask[idx]) {
            float checked_diff = fabsf(ctx->buffer[idx] - SILENT_ANGLE);
            float last_diff = fabsf(last_checked - SILENT_ANGLE);
            if (checked_diff < last_diff) {
                moving_towards_90++;
            }
            last_checked = ctx->buffer[idx];
        }
    }
    
    return (moving_towards_90 >= 3);
}

/**
 * @brief  Quantize angle to nearest interval center (20-degree intervals)
 */
static float quantize_angle(float angle)
{
    if (angle < ANGLE_MIN) {
        angle = ANGLE_MIN;
    } else if (angle > ANGLE_MAX) {
        angle = ANGLE_MAX;
    }
    
    int interval = (int)(angle / ANGLE_QUANTIZATION_STEP);
    if (interval >= 9) {
        interval = 8;  // Handle 180 degrees
    }
    
    return interval * ANGLE_QUANTIZATION_STEP + ANGLE_QUANTIZATION_STEP / 2.0f;
}

/**
 * @brief  Check if an angle is valid (not silent)
 */
static bool is_angle_valid(float angle, audio_doa_tracker_ctx_t *ctx, float avg_angle, bool has_valid)
{
    // Non-90 angles are always valid
    if (!is_near_90_degrees(angle)) {
        reset_90_tracking(ctx);
        return true;
    }
    
    // Front-facing mode: accept all 90-degree angles
    if (ctx->is_front_facing_mode) {
        return true;
    }
    
    // Check continuous 90-degree duration
    start_90_tracking(ctx);
    if (check_continuous_90_duration(ctx)) {
        return true;
    }
    
    // During initial collection, accept to allow detection
    if (ctx->valid_count < INITIAL_SAMPLES_TO_CHECK) {
        return true;
    }
    
    // Check angle change pattern
    if (!ctx->has_last_valid_angle) {
        // No history - be conservative
        return buffer_mostly_90(ctx);
    }
    
    bool last_was_near_90 = is_near_90_degrees(ctx->last_valid_angle);
    
    if (last_was_near_90) {
        // Stable around 90 degrees
        float angle_change = fabsf(angle - ctx->last_valid_angle);
        return (angle_change < GRADUAL_CHANGE_THRESHOLD);
    }
    
    // Changed from non-90 to 90
    reset_90_tracking(ctx);
    
    // Check if gradual change with trend
    if (check_gradual_change_to_90(angle, ctx)) {
        return true;
    }
    
    // Check buffer context
    if (ctx->is_not_front_facing_detected) {
        // Initial samples were NOT near 90 - be strict
        return buffer_mostly_90(ctx);
    }
    
    // Default: check buffer
    return buffer_mostly_90(ctx);
}

/**
 * @brief  Check initial samples to detect front-facing speech mode
 */
static void check_initial_samples(audio_doa_tracker_ctx_t *ctx)
{
    if (ctx->initial_samples_count >= INITIAL_SAMPLES_TO_CHECK ||
        ctx->valid_count < INITIAL_SAMPLES_TO_CHECK) {
        return;
    }
    
    int near_90_count = 0;
    int checked = 0;
    
    for (int i = 0; i < DOA_TRACKER_BUFFER_SIZE && checked < INITIAL_SAMPLES_TO_CHECK; i++) {
        if (ctx->valid_mask[i]) {
            if (is_near_90_degrees(ctx->original_buffer[i])) {
                near_90_count++;
            }
            checked++;
        }
    }
    
    if (checked >= INITIAL_SAMPLES_TO_CHECK) {
        if (near_90_count >= INITIAL_SAMPLES_TO_CHECK) {
            ctx->is_front_facing_mode = true;
            // ESP_LOGI(TAG, "Front-facing mode detected (%d/%d samples near 90)", near_90_count, checked);
        } else {
            ctx->is_not_front_facing_detected = true;
            // ESP_LOGI(TAG, "Not front-facing mode (%d/%d samples near 90)", near_90_count, checked);
        }
        ctx->initial_samples_count = INITIAL_SAMPLES_TO_CHECK;
    }
}

/**
 * @brief  Apply bias to angle based on range
 */
static float apply_angle_bias(float avg_angle, float min_angle, float max_angle)
{
    if (avg_angle >= 110.0f && avg_angle <= 180.0f) {
        return avg_angle * 0.3f + max_angle * 0.7f;  // Bias towards larger
    } else if (avg_angle >= 0.0f && avg_angle <= 40.0f) {
        return avg_angle * 0.3f + min_angle * 0.7f;  // Bias towards smaller
    }
    return avg_angle;
}

/**
 * @brief  Calculate first output angle with bias
 */
static float calculate_first_output_angle(audio_doa_tracker_ctx_t *ctx)
{
    if (ctx->valid_count == 0) {
        return 0.0f;
    }
    
    float sum = 0.0f;
    float min_angle = 180.0f;
    float max_angle = 0.0f;
    int count = 0;
    
    for (int i = 0; i < DOA_TRACKER_BUFFER_SIZE; i++) {
        if (ctx->valid_mask[i]) {
            float val = ctx->buffer[i];
            sum += val;
            if (val < min_angle) min_angle = val;
            if (val > max_angle) max_angle = val;
            count++;
        }
    }
    
    if (count == 0) {
        return 0.0f;
    }
    
    return apply_angle_bias(sum / count, min_angle, max_angle);
}

/**
 * @brief  Calculate weighted average angle with bias
 */
static float calculate_average_angle(audio_doa_tracker_ctx_t *ctx)
{
    if (ctx->valid_count == 0) {
        return 0.0f;
    }
    
    float weighted_sum = 0.0f;
    float total_weight = 0.0f;
    float min_angle = 180.0f;
    float max_angle = 0.0f;
    
    int latest_idx = (ctx->write_index - 1 + DOA_TRACKER_BUFFER_SIZE) % DOA_TRACKER_BUFFER_SIZE;
    
    for (int i = 0; i < DOA_TRACKER_BUFFER_SIZE; i++) {
        if (ctx->valid_mask[i]) {
            float weight = (i == latest_idx) ? RECENT_WEIGHT_FACTOR : 1.0f;
            float val = ctx->buffer[i];
            weighted_sum += val * weight;
            total_weight += weight;
            
            if (val < min_angle) min_angle = val;
            if (val > max_angle) max_angle = val;
        }
    }
    
    if (total_weight == 0.0f) {
        return 0.0f;
    }
    
    return apply_angle_bias(weighted_sum / total_weight, min_angle, max_angle);
}

/**
 * @brief  Check if 90-degree output should be allowed
 */
static bool should_allow_90_output(audio_doa_tracker_ctx_t *ctx, TickType_t current_tick)
{
    // Check if buffer has mostly real 90-degree values
    int near_90_count = count_near_90_in_buffer(ctx);
    int total_valid = ctx->valid_count;
    
    if (near_90_count < (int)(total_valid * BUFFER_90_RATIO_THRESHOLD)) {
        ESP_LOGD(TAG, "Average is 90 but only %d/%d samples in 84-96 range", near_90_count, total_valid);
        return false;
    }
    
    // If front-facing mode, allow output
    if (ctx->is_front_facing_mode) {
        return true;
    }
    
    // Check continuous duration
    if (!ctx->has_near_90_start ||
        (current_tick - ctx->first_near_90_tick) < pdMS_TO_TICKS(CONTINUOUS_90_DURATION_MS)) {
        ESP_LOGD(TAG, "Average is 90 but not continuous %d ms", CONTINUOUS_90_DURATION_MS);
        return false;
    }
    
    return true;
}

/**
 * @brief  Reset tracker state
 */
static void reset_tracker_state(audio_doa_tracker_ctx_t *ctx)
{
    ctx->write_index = 0;
    ctx->valid_count = 0;
    ctx->is_front_facing_mode = false;
    ctx->is_not_front_facing_detected = false;
    ctx->initial_samples_count = 0;
    ctx->last_valid_angle = 0.0f;
    ctx->has_last_valid_angle = false;
    ctx->last_output_angle = 0.0f;
    ctx->has_output_angle = false;
    reset_90_tracking(ctx);
    ctx->last_output_tick = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
    memset(ctx->original_buffer, 0, sizeof(ctx->original_buffer));
    memset(ctx->valid_mask, 0, sizeof(ctx->valid_mask));
}

esp_err_t audio_doa_tracker_init(audio_doa_tracker_cfg_t *cfg, audio_doa_tracker_handle_t *out_handle)
{
    if (cfg == NULL || out_handle == NULL || cfg->result_callback == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_doa_tracker_ctx_t *ctx = (audio_doa_tracker_ctx_t *)calloc(1, sizeof(audio_doa_tracker_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    
    ctx->enabled = false;
    ctx->output_interval_ms = (cfg->output_interval_ms > 0) ? cfg->output_interval_ms : 0;
    ctx->min_angle_change_threshold = (cfg->min_angle_change_threshold > 0.0f) ? cfg->min_angle_change_threshold : 5.0f;
    ctx->result_callback = cfg->result_callback;
    ctx->ctx = cfg->ctx;
    reset_tracker_state(ctx);
    
    *out_handle = (audio_doa_tracker_handle_t)ctx;
    ESP_LOGI(TAG, "DOA tracker initialized");
    return ESP_OK;
}

esp_err_t audio_doa_tracker_feed(audio_doa_tracker_handle_t handle, float angle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_doa_tracker_ctx_t *ctx = (audio_doa_tracker_handle_t)handle;
    if (!ctx->enabled) {
        return ESP_OK;
    }
    
    // Validate angle before quantization
    float current_avg = calculate_average_angle(ctx);
    bool has_valid_samples = (ctx->valid_count > 0);
    
    if (!is_angle_valid(angle, ctx, current_avg, has_valid_samples)) {
        return ESP_OK;  // Invalid angle, skip
    }
    
    // Quantize angle
    float quantized_angle = quantize_angle(angle);
    
    // Check for major angle change - reset buffer if needed
    if (has_valid_samples && ctx->valid_count >= DOA_TRACKER_BUFFER_SIZE) {
        if (fabsf(angle - current_avg) > MAJOR_ANGLE_CHANGE_THRESHOLD) {
            reset_tracker_state(ctx);
            ESP_LOGD(TAG, "Major angle change detected, resetting buffer");
        }
    }
    
    // Add to buffer
    if (!ctx->valid_mask[ctx->write_index]) {
        ctx->valid_count++;
    }
    
    ctx->buffer[ctx->write_index] = quantized_angle;
    ctx->original_buffer[ctx->write_index] = angle;
    ctx->valid_mask[ctx->write_index] = true;
    ctx->write_index = (ctx->write_index + 1) % DOA_TRACKER_BUFFER_SIZE;
    
    ctx->last_valid_angle = quantized_angle;
    ctx->has_last_valid_angle = true;
    
    check_initial_samples(ctx);
    
    // Output logic
    TickType_t current_tick = xTaskGetTickCount();
    bool should_output = false;
    float avg_angle = 0.0f;
    
    if (!ctx->has_output_angle) {
        // First output: wait for buffer to fill
        if (ctx->valid_count >= DOA_TRACKER_BUFFER_SIZE) {
            avg_angle = calculate_first_output_angle(ctx);
            should_output = true;
        }
    } else {
        // Subsequent outputs
        if (ctx->valid_count >= DOA_TRACKER_BUFFER_SIZE) {
            // Check timing
            if (ctx->output_interval_ms == 0 ||
                (current_tick - ctx->last_output_tick) >= pdMS_TO_TICKS(ctx->output_interval_ms)) {
                avg_angle = calculate_average_angle(ctx);
                
                // Special check for 90-degree output
                if (fabsf(avg_angle - SILENT_ANGLE) < 5.0f) {
                    should_output = should_allow_90_output(ctx, current_tick);
                } else {
                    should_output = true;
                }
                
                // Check angle change thresholds
                if (should_output) {
                    float angle_change = fabsf(avg_angle - ctx->last_output_angle);
                    
                    // Check if change is too large (unreasonable jump)
                    if (angle_change > REASONABLE_CHANGE_THRESHOLD) {
                        should_output = false;
                        ESP_LOGD(TAG, "Angle change too large (%.1f -> %.1f, diff=%.1f)", 
                                 ctx->last_output_angle, avg_angle, angle_change);
                    }
                    // Check if change is too small (less than minimum threshold)
                    else if (ctx->min_angle_change_threshold > 0.0f && 
                             angle_change < ctx->min_angle_change_threshold) {
                        should_output = false;
                        ESP_LOGD(TAG, "Angle change too small (%.1f -> %.1f, diff=%.1f < %.1f)", 
                                 ctx->last_output_angle, avg_angle, angle_change, ctx->min_angle_change_threshold);
                    }
                }
            }
        }
    }
    
    if (should_output) {
        ctx->last_output_angle = avg_angle;
        ctx->has_output_angle = true;
        ctx->last_output_tick = current_tick;
        
        if (ctx->result_callback) {
            ctx->result_callback(avg_angle, ctx->ctx);
        }
    }
    
    return ESP_OK;
}

esp_err_t audio_doa_tracker_enable(audio_doa_tracker_handle_t handle, bool enable)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_doa_tracker_ctx_t *ctx = (audio_doa_tracker_handle_t)handle;
    ctx->enabled = enable;
    
    if (enable) {
        reset_tracker_state(ctx);
        ESP_LOGI(TAG, "DOA tracker enabled");
    } else {
        reset_tracker_state(ctx);
        ESP_LOGI(TAG, "DOA tracker disabled");
    }
    
    return ESP_OK;
}

esp_err_t audio_doa_tracker_deinit(audio_doa_tracker_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    free((audio_doa_tracker_ctx_t *)handle);
    return ESP_OK;
}
