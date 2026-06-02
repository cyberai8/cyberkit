#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "audio_doa_app.h"
#include "audio_doa.h"
#include "audio_doa_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

static const char *TAG = "audio_doa_app";

typedef struct {
    audio_doa_handle_t                          doa_handle;
    audio_doa_tracker_handle_t                  doa_tracker_handle;
    audio_doa_monitor_callback_t                audio_doa_monitor_callback;
    void*                                       audio_doa_monitor_callback_ctx;
    struct {
        bool vad_detect : 1;
    }flags;
} audio_doa_app_t;

static void audio_doa_callback(float angle, void *ctx)
{
    audio_doa_app_t *app = (audio_doa_app_t *)ctx;
    if (app == NULL) {
        ESP_LOGE(TAG, "audio_doa_callback: app is NULL");
        return;
    }
    if (app->doa_tracker_handle == NULL) {
        ESP_LOGE(TAG, "audio_doa_callback: doa_tracker_handle is NULL");
        return;
    }
    audio_doa_tracker_feed(app->doa_tracker_handle, angle);

    // ESP_LOGI(TAG, "audio_doa_callback: angle %.2f", angle);//注释

    if (app->audio_doa_monitor_callback != NULL) {
        app->audio_doa_monitor_callback(angle, app->audio_doa_monitor_callback_ctx);
    }
}

esp_err_t audio_doa_app_create(audio_doa_app_handle_t *handle, audio_doa_app_config_t *config)
{
    if (handle == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *handle = (audio_doa_app_handle_t)calloc(1, sizeof(audio_doa_app_t));
    if (*handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_doa_app_t *app = (audio_doa_app_t *)*handle;
    app->doa_handle = NULL;
    app->doa_tracker_handle = NULL;

    esp_err_t ret = ESP_OK;
    audio_doa_config_t doa_cfg = {
        .distance = config->distance,
    };
    ret = audio_doa_new(&app->doa_handle, &doa_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    app->audio_doa_monitor_callback = config->audio_doa_monitor_callback;
    app->audio_doa_monitor_callback_ctx = config->audio_doa_monitor_callback_ctx;
    audio_doa_set_doa_result_callback(app->doa_handle, audio_doa_callback, (void *)app);

    audio_doa_tracker_cfg_t doa_tracker_cfg = {
        .result_callback = config->audio_doa_result_callback,
        .ctx = config->audio_doa_result_callback_ctx,
        .output_interval_ms = 1000,
    };
    ret = audio_doa_tracker_init(&doa_tracker_cfg, &app->doa_tracker_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_doa_app_start(*handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "audio_doa_app_create success");

    return ESP_OK;
}

esp_err_t audio_doa_app_destroy(audio_doa_app_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_app_t *app = (audio_doa_app_t *)handle;

    esp_err_t ret = ESP_OK;
    ret = audio_doa_delete(app->doa_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_doa_tracker_deinit(app->doa_tracker_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    free(app);
    return ESP_OK;
}

esp_err_t audio_doa_app_start(audio_doa_app_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_app_t *app = (audio_doa_app_t *)handle;

    esp_err_t ret = ESP_OK;
    ret = audio_doa_start(app->doa_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_doa_tracker_enable(app->doa_tracker_handle, true);
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_OK;
}

esp_err_t audio_doa_app_stop(audio_doa_app_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_app_t *app = (audio_doa_app_t *)handle;

    esp_err_t ret = ESP_OK;
    ret = audio_doa_stop(app->doa_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_doa_tracker_enable(app->doa_tracker_handle, false);
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_OK;
}

esp_err_t audio_doa_app_data_write(audio_doa_app_handle_t handle, uint8_t *data, int bytes_size)
{
    if (handle == NULL || data == NULL || bytes_size <= 0) {
        ESP_LOGE(TAG, "audio_doa_app_data_write: invalid args");
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_app_t *app = (audio_doa_app_t *)handle;

    if (app->flags.vad_detect == false) {
        return ESP_OK;
    }

    return audio_doa_data_write(app->doa_handle, data, bytes_size);
}

esp_err_t audio_doa_app_set_vad_detect(audio_doa_app_handle_t handle, bool vad_detect)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_doa_app_t *app = (audio_doa_app_t *)handle;
    app->flags.vad_detect = vad_detect;

    return ESP_OK;
}

#ifdef __cplusplus
}
#endif  /* __cplusplus */