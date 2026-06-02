#include "local_http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "application.h"
#include <string>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "LOCAL_HTTP_SERVER";
static httpd_handle_t server = NULL;

/*
 * 处理 POST /tts 请求的回调函数
 */
static esp_err_t tts_post_handler(httpd_req_t *req) {
    int total_len = req->content_len;
    int cur_len = 0;
    
    // 如果没有内容
    if (total_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }

    // 分配内存接收 JSON 数据
    char *buf = (char *)malloc(total_len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP request body");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            // 如果超时，可以继续重试
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0'; // 确保字符串以 null 结尾

    // 解析 JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON data: %s", buf);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }

    // 获取 "text" 字段
    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item != NULL && cJSON_IsString(text_item)) {
        ESP_LOGI(TAG, "Received TTS text for playback: %s", text_item->valuestring);
        
        // 调用 Application 的接口，模拟用户输入，让服务器进行 TTS 播报
        // 唤醒词不能传长文本，这里根据收到的文本前缀发送固定的短文本
        std::string received_text = text_item->valuestring;
        std::string command;
        
        if (received_text.find("主人，您交代的任务已经有结果了") != std::string::npos || 
            received_text.find("主人，您交代的任务有结果了") != std::string::npos ||
            received_text.find("主人，任务已完成。") != std::string::npos) {
            command = "请复述：任务成功";
        } else if (received_text.find("主人，您交代的任务执行遇到了问题") != std::string::npos ||
            received_text.find("主人，任务执行失败。") != std::string::npos) {
            command = "请复述：任务失败了";
        }
        
        if (!command.empty()) {
            Application::GetInstance().TriggerWakeWord(command);
        }

    } else {
        ESP_LOGW(TAG, "JSON missing 'text' field or is not a string");
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text' field");
        return ESP_FAIL;
    }

    // 释放资源
    cJSON_Delete(root);
    free(buf);

    // 响应客户端
    const char *resp = "{\"status\": \"success\", \"message\": \"TTS request received\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    
    return ESP_OK;
}

// 注册 /tts URI 结构体
static const httpd_uri_t tts_uri = {
    .uri       = "/tts",
    .method    = HTTP_POST,
    .handler   = tts_post_handler,
    .user_ctx  = NULL
};

esp_err_t start_local_http_server(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    // 增加栈大小防止 cJSON 解析等操作导致栈溢出
    config.stack_size = 8192; 

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers...");
        httpd_register_uri_handler(server, &tts_uri);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

void stop_local_http_server(void) {
    if (server != NULL) {
        ESP_LOGI(TAG, "Stopping HTTP Server");
        httpd_stop(server);
        server = NULL;
    }
}
