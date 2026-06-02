#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动本地 HTTP 服务器，用于接收来自局域网的主动推送通知
 * 
 * @return esp_err_t ESP_OK 表示启动成功，其他表示失败
 */
esp_err_t start_local_http_server(void);

/**
 * @brief 停止本地 HTTP 服务器并释放资源
 */
void stop_local_http_server(void);

#ifdef __cplusplus
}
#endif
