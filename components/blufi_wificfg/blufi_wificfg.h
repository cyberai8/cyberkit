#ifndef __BLUFI_APP_H__
#define __BLUFI_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"

#include "esp_wifi.h"

typedef enum {
    BLUFI_ERROR_WIFI_PASSWORD_WRONG = 1,
    BLUFI_ERROR_WIFI_NETWORK_UNAVAILABLE = 2,
    BLUFI_ERROR_WIFI_CONNECTION_TIMEOUT = 3,
    BLUFI_ERROR_OTA_CHECK_FAILED = 4,
    BLUFI_ERROR_OTA_CHECK_TIMEOUT = 5,
    BLUFI_ERROR_BLE_DISCONNECTED = 6,
    BLUFI_ERROR_WIFI_CONNECTION_FAILED = 7,
} blufi_wificfg_error_t;

typedef struct {
    void (*sta_config_cb)(const wifi_config_t *wifi_config, void *arg);
    void (*custom_data_cb)(const uint8_t *data, size_t len, void *arg);
    void (*error_cb)(blufi_wificfg_error_t error, const char *message, void *arg);
}blufi_wificfg_cbs_t;

esp_err_t blufi_wificfg_send_custom(uint8_t *data, size_t len);

esp_err_t blufi_wificfg_send_error_message(const char *error_msg);

esp_err_t blufi_wificfg_start(bool init_wifi, char *device_name, blufi_wificfg_cbs_t cbs, void *cbs_arg);

esp_err_t blufi_wificfg_stop(void);

bool blufi_wificfg_is_ble_connected(void);

#ifdef __cplusplus
}
#endif

#endif //__BLUFI_APP_H__