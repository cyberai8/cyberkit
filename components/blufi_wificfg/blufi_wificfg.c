#include "sdkconfig.h"
#ifdef CONFIG_BLUFI_WIFICFG_ENABLED
#include "blufi_wificfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "esp_blufi_api.h"

#include "esp_blufi.h"

#include "blufi_log.h"

#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY 10
#define EXAMPLE_INVALID_REASON                255
#define EXAMPLE_INVALID_RSSI                  -128
#define WIFI_CONNECTION_TIMEOUT_MS            30000  // 30秒超时
#define OTA_CHECK_TIMEOUT_MS                  60000  // 60秒超时

void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

int blufi_security_init(void);
void blufi_security_deinit(void);
int esp_blufi_gap_register_callback(void);
void esp_blufi_set_device_name(char *device_name);
esp_err_t esp_blufi_host_init(void);
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *callbacks);
esp_err_t esp_blufi_host_deinit(void);
esp_err_t esp_blufi_controller_init(void);
esp_err_t esp_blufi_controller_deinit(void);

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);
static void example_record_wifi_conn_info(int rssi, uint8_t reason);
static int softap_get_current_connection_number(void);
static void reset_wifi_connection_state(void);

#define WIFI_LIST_NUM   10

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t example_wifi_retry = 0;

/* store the station info for send back to phone */
static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;
static int64_t gl_wifi_connect_start_time = 0;
static bool gl_wifi_config_received = false;

// 重置WiFi连接状态，为下次配网做准备
static void reset_wifi_connection_state(void)
{
    gl_sta_connected = false;
    gl_sta_got_ip = false;
    gl_sta_is_connecting = false;
    gl_wifi_connect_start_time = 0;
    memset(gl_sta_ssid, 0, sizeof(gl_sta_ssid));
    memset(gl_sta_bssid, 0, sizeof(gl_sta_bssid));
    gl_sta_ssid_len = 0;
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (wifi_event_group) {
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    BLUFI_INFO("WiFi connection state reset for next configuration");
}

static blufi_wificfg_cbs_t g_cbs = {0};
static void *g_cbs_arg = NULL;
static TaskHandle_t g_timeout_task_handle = NULL;
static bool g_is_stopping = false;

static void wifi_connection_timeout_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒检查一次
        
        if (gl_sta_is_connecting && gl_wifi_connect_start_time > 0) {
            int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            int64_t elapsed = current_time - gl_wifi_connect_start_time;
            
            if (elapsed > WIFI_CONNECTION_TIMEOUT_MS) {
                BLUFI_ERROR("WiFi connection timeout after %lld ms", elapsed);
                
                // 断开WiFi连接
                esp_wifi_disconnect();
                
                // 如果BLE已连接，发送超时错误给设备端
                if (ble_is_connected && gl_wifi_config_received) {
                    wifi_mode_t mode;
                    esp_wifi_get_mode(&mode);
                    
                    const char* error_msg = "WIFI_CONNECTION_TIMEOUT";
                    blufi_wificfg_send_error_message(error_msg);
                    
                    // 记录超时错误信息
                    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, WIFI_REASON_CONNECTION_FAIL);
                    esp_blufi_extra_info_t info;
                    memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                    info.sta_conn_end_reason_set = true;
                    info.sta_conn_end_reason = WIFI_REASON_CONNECTION_FAIL;
                    esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 
                                                    softap_get_current_connection_number(), 
                                                    &info);
                    
                    // 调用错误回调
                    if (g_cbs.error_cb) {
                        g_cbs.error_cb(BLUFI_ERROR_WIFI_CONNECTION_TIMEOUT, error_msg, g_cbs_arg);
                    }
                }
                
                // 重置连接状态，但保持配置接收标志，允许设备端重新尝试
                reset_wifi_connection_state();
                // 保持 gl_wifi_config_received = true，允许设备端修改配置后重新尝试
            }
        }
    }
}

static void example_record_wifi_conn_info(int rssi, uint8_t reason)
{
    static bool wifi_is_started = false;
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (gl_sta_is_connecting) {
        gl_sta_conn_info.sta_max_conn_retry_set = true;
        gl_sta_conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }

    if(wifi_is_started == false) {
        wifi_is_started = true;
        esp_wifi_start();
    }
}

static void example_wifi_connect(void)
{
    example_wifi_retry = 0;
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    gl_wifi_connect_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
}

static const char* wifi_reason_to_string(wifi_err_reason_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "WiFi password incorrect or authentication failed";
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NOT_AUTHED:
        case WIFI_REASON_NOT_ASSOCED:
            return "WiFi network unavailable or AP not found";
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_CONNECTION_FAIL:
            return "WiFi connection failed";
        default:
            return "WiFi connection error";
    }
}

static bool example_wifi_reconnect(void)
{
    bool ret;
    if (gl_sta_is_connecting && example_wifi_retry++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
        BLUFI_INFO("BLUFI WiFi starts reconnection\n");
        gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
        example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}

static int softap_get_current_connection_number(void)
{
    esp_err_t ret;
    ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
    if (ret == ESP_OK)
    {
        return gl_sta_list.num;
    }

    return 0;
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_mode_t mode;

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        esp_blufi_extra_info_t info;

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        esp_wifi_get_mode(&mode);

        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, gl_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid;
        info.sta_ssid_len = gl_sta_ssid_len;
        gl_sta_got_ip = true;
        gl_wifi_connect_start_time = 0; // 清除超时计时，连接成功
        if (ble_is_connected == true) {
            if(g_cbs.sta_config_cb) {
                g_cbs.sta_config_cb(&sta_config, g_cbs_arg);
            }
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_get_current_connection_number(), &info);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        break;
    }
    default:
        break;
    }
    return;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;
    wifi_mode_t mode;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        example_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        gl_sta_connected = true;
        gl_sta_is_connecting = false;
        gl_wifi_connect_start_time = 0; // 清除超时计时
        event = (wifi_event_sta_connected_t*) event_data;
        memcpy(gl_sta_bssid, event->bssid, 6);
        memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
        gl_sta_ssid_len = event->ssid_len;
        BLUFI_INFO("WiFi connected to SSID: %.*s", gl_sta_ssid_len, gl_sta_ssid);
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
        BLUFI_INFO("WiFi disconnected reason=%d (%s)", disconnected_event->reason, wifi_reason_to_string(disconnected_event->reason));
        /* Only handle reconnection during connecting */
        if (gl_sta_connected == false && example_wifi_reconnect() == false) {
            // 重试失败，记录错误信息
            example_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
            
            // 如果BLE已连接，发送错误信息给设备端
            if (ble_is_connected && gl_wifi_config_received) {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                
                // 判断错误类型
                const char* error_code = NULL;
                if (disconnected_event->reason == WIFI_REASON_AUTH_FAIL ||
                    disconnected_event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    disconnected_event->reason == WIFI_REASON_AUTH_EXPIRE ||
                    disconnected_event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                    disconnected_event->reason == WIFI_REASON_ASSOC_FAIL) {
                    error_code = "WIFI_AUTH_FAILED";
                } else if (disconnected_event->reason == WIFI_REASON_NO_AP_FOUND ||
                           disconnected_event->reason == WIFI_REASON_NOT_AUTHED ||
                           disconnected_event->reason == WIFI_REASON_NOT_ASSOCED) {
                    error_code = "WIFI_NETWORK_UNAVAILABLE";
                } else {
                    error_code = "WIFI_CONNECTION_FAILED";
                }
                blufi_wificfg_send_error_message(error_code);
                
                // 发送连接失败报告
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 
                                                softap_get_current_connection_number(), 
                                                &gl_sta_conn_info);
                
                // 调用错误回调
                if (g_cbs.error_cb) {
                    if (disconnected_event->reason == WIFI_REASON_AUTH_FAIL || 
                        disconnected_event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                        disconnected_event->reason == WIFI_REASON_AUTH_EXPIRE ||
                        disconnected_event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                        disconnected_event->reason == WIFI_REASON_ASSOC_FAIL) {
                        g_cbs.error_cb(BLUFI_ERROR_WIFI_PASSWORD_WRONG, error_code, g_cbs_arg);
                    } else if (disconnected_event->reason == WIFI_REASON_NO_AP_FOUND ||
                               disconnected_event->reason == WIFI_REASON_NOT_AUTHED ||
                               disconnected_event->reason == WIFI_REASON_NOT_ASSOCED) {
                        g_cbs.error_cb(BLUFI_ERROR_WIFI_NETWORK_UNAVAILABLE, error_code, g_cbs_arg);
                    } else {
                        g_cbs.error_cb(BLUFI_ERROR_WIFI_CONNECTION_FAILED, error_code, g_cbs_arg);
                    }
                }
            }
            
            // 重置连接状态，允许接收新的配置并重新尝试
            reset_wifi_connection_state();
            // 保持 gl_wifi_config_received = true，允许设备端重新发送配置
            // 这样设备端可以修改密码后重新尝试
        } else if (gl_sta_connected == true) {
            // 已连接后断开，重置状态
            reset_wifi_connection_state();
        }
        break;
    case WIFI_EVENT_AP_START:
        esp_wifi_get_mode(&mode);

        /* TODO: get config or information of softap, then set to report extra_info */
        if (ble_is_connected == true) {
            if (gl_sta_connected) {
                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, gl_sta_bssid, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = gl_sta_ssid;
                info.sta_ssid_len = gl_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
            } else if (gl_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
            }
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        break;
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);

        // 添加内存监控日志
        BLUFI_INFO("Before malloc: Free heap=%d, Min free=%d, AP count=%d",
                   heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                   heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
                   apCount);

        if (apCount == 0) {
            BLUFI_INFO("Nothing AP found");
            break;
        }
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list) {
            BLUFI_ERROR("malloc error, ap_list is NULL (size=%d bytes)",
                        sizeof(wifi_ap_record_t) * apCount);
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t * blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list) {
            if (ap_list) {
                free(ap_list);
            }
            BLUFI_ERROR("malloc error, blufi_ap_list is NULL (size=%d bytes)",
                        apCount * sizeof(esp_blufi_ap_record_t));
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
            // 确保SSID字符串有null终止符，防止读取越界导致乱码
            blufi_ap_list[i].ssid[sizeof(blufi_ap_list[i].ssid) - 1] = '\0';
        }

        if (ble_is_connected == true) {
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }

        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        // wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        // BLUFI_INFO("station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        // wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        // BLUFI_INFO("station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        break;
    }

    default:
        break;
    }
    return;
}

#ifdef CONFIG_EXAMPLE_USE_SC
static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};
#else
static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
};
#endif

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    /* actually, should post to blufi_task handle the procedure,
     * now, as a example, we do it more simply */

    BLUFI_INFO("event: %d\n", event);

    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish\n");

        esp_blufi_adv_start();
        BLUFI_INFO("BLUFI adv start called\n");
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish\n");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        BLUFI_INFO("BLUFI ble connect\n");
        ble_is_connected = true;
        // 重置WiFi配置接收标志，允许重新配网
        gl_wifi_config_received = false;
        esp_blufi_adv_stop();
#ifdef CONFIG_EXAMPLE_USE_SC
        blufi_security_init();
#endif
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect\n");
        if (g_is_stopping) {
            ble_is_connected = false;
            reset_wifi_connection_state();
            gl_wifi_config_received = false;
#ifdef CONFIG_EXAMPLE_USE_SC
            blufi_security_deinit();
#endif
            break;
        }
        
        // 在BLE断开前，如果WiFi连接失败，尝试发送错误消息
        if (gl_sta_is_connecting && gl_wifi_config_received) {
            // WiFi is connecting but not yet successful, send failure code
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 
                                            softap_get_current_connection_number(), 
                                            &gl_sta_conn_info);
            blufi_wificfg_send_error_message("BLE_DISCONNECTED");
        }
        
        ble_is_connected = false;
        // 重置所有状态，允许重新连接时重新配网
        reset_wifi_connection_state();
        gl_wifi_config_received = false;  // 清除配置接收标志，需要重新接收配置
        
#ifdef CONFIG_EXAMPLE_USE_SC
        blufi_security_deinit();
#endif
        esp_blufi_adv_start();
        
        // 调用错误回调通知上层
        if (g_cbs.error_cb) {
            g_cbs.error_cb(BLUFI_ERROR_BLE_DISCONNECTED, "BLE disconnected", g_cbs_arg);
        }
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK( esp_wifi_set_mode(param->wifi_mode.op_mode) );
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        BLUFI_INFO("BLUFI request wifi connect to AP\n");
        /* there is no wifi callback when the device has already connected to this wifi
        so disconnect wifi before connection.
        */
        // 重置之前的连接状态
        reset_wifi_connection_state();
        esp_wifi_disconnect();
        example_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI request wifi disconnect from AP\n");
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (gl_sta_connected) {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
        } else if (gl_sta_is_connecting) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
        } else {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
        }
        BLUFI_INFO("BLUFI get wifi status from AP\n");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        /* TODO */
        break;
	case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA BSSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(sta_config.sta.ssid)/sizeof(sta_config.sta.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA SSID\n");
            break;
        }
        // 接收到新的SSID，重置之前的连接状态，允许重新配网
        if (gl_wifi_config_received) {
            BLUFI_INFO("Received new SSID, resetting previous connection state");
            reset_wifi_connection_state();
            esp_wifi_disconnect();  // 断开当前连接
        }
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        gl_wifi_config_received = true; // 标记已收到WiFi配置
        BLUFI_INFO("Recv STA SSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(sta_config.sta.password)/sizeof(sta_config.sta.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA PASSWORD\n");
            break;
        }
        // 接收到新的密码，如果之前连接失败，重置状态允许重新尝试
        if (gl_wifi_config_received && (gl_sta_is_connecting || !gl_sta_connected)) {
            BLUFI_INFO("Received new password, resetting connection state for retry");
            reset_wifi_connection_state();
            esp_wifi_disconnect();  // 断开当前连接
        }
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        gl_wifi_config_received = true; // 标记已收到WiFi配置
        BLUFI_INFO("Recv STA PASSWORD len=%d\n", param->sta_passwd.passwd_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        if (param->softap_ssid.ssid_len >= sizeof(ap_config.ap.ssid)/sizeof(ap_config.ap.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid SOFTAP SSID\n");
            break;
        }
        strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
        ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid, ap_config.ap.ssid_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        if (param->softap_passwd.passwd_len >= sizeof(ap_config.ap.password)/sizeof(ap_config.ap.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid SOFTAP PASSWD\n");
            break;
        }
        strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP PASSWORD %s len = %d\n", ap_config.ap.password, param->softap_passwd.passwd_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num > 4) {
            return;
        }
        ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
            return;
        }
        ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel > 13) {
            return;
        }
        ap_config.ap.channel = param->softap_channel.channel;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:{
         BLUFI_INFO("GET_WIFI_LIST");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_disconnect();

        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK) {
            BLUFI_ERROR("BLUFI wifi scan fail");
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
        // ESP_LOG_BUFFER_HEX("Custom Data", param->custom_data.data, param->custom_data.data_len);
        if(g_cbs.custom_data_cb) {
            g_cbs.custom_data_cb(param->custom_data.data, param->custom_data.data_len, g_cbs_arg);
        }
        break;
	case ESP_BLUFI_EVENT_RECV_USERNAME:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CA_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        /* Not handle currently */
        break;;
	case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        /* Not handle currently */
        break;
    default:
        break;
    }
}

esp_err_t blufi_wificfg_send_custom(uint8_t *data, size_t len) {
    if(ble_is_connected == false){
        BLUFI_ERROR("BLUFI BLE is not connected yet");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_blufi_send_custom_data((uint8_t *)data, len);
    if(err != ESP_OK){
        BLUFI_ERROR("BLUFI send custom failed: %s", esp_err_to_name(err));
        return err;
    }
    BLUFI_INFO("BLUFI send custom success");
    return ESP_OK;
}

esp_err_t blufi_wificfg_send_error_message(const char *error_msg) {
    if (error_msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Directly send the given error code string (already in unified format like OTA_CHECK_TIMEOUT)
    return blufi_wificfg_send_custom((uint8_t*)error_msg, strlen(error_msg));
}

bool blufi_wificfg_is_ble_connected(void) {
    return ble_is_connected;
}

esp_err_t blufi_wificfg_start(bool init_wifi, char *device_name, blufi_wificfg_cbs_t cbs, void *cbs_arg) {
    esp_err_t ret;

    if (init_wifi)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);
        esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
        assert(ap_netif);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        esp_wifi_start();
    }

    g_cbs = cbs;
    g_cbs_arg = cbs_arg;
    g_is_stopping = false;

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    
    // 创建超时检查任务
    if (g_timeout_task_handle == NULL) {
        xTaskCreate(wifi_connection_timeout_task, "wifi_timeout", 2048, NULL, 5, &g_timeout_task_handle);
    }

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret) {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
#endif
    esp_blufi_set_device_name(device_name);

    ret = esp_blufi_host_and_cb_init(&example_callbacks);
    if (ret) {
        BLUFI_ERROR("%s initialise failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t blufi_wificfg_stop(void){
    esp_err_t ret;
    g_is_stopping = true;

    // 删除超时检查任务
    if (g_timeout_task_handle != NULL) {
        vTaskDelete(g_timeout_task_handle);
        g_timeout_task_handle = NULL;
    }

    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler);
    
    // 完全重置所有状态
    reset_wifi_connection_state();
    gl_wifi_config_received = false;
    ble_is_connected = false;

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_deinit();
    if (ret) {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
#endif

    ret = esp_blufi_host_deinit();
    return ret;
}
#endif
