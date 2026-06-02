#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <font_awesome.h>
#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

#include "mqtt_protocol.h"
#include "websocket_protocol.h"

#include <memory>
#include "display/lcd_display.h"
#ifdef CONFIG_USE_BLUFI_NET_CONFIGURING
#include "esp_mac.h"
#include "blufi_wificfg.h"
#endif

static const char *TAG = "WifiBoard";

static void reboot_timer_cb(void* arg) {
    esp_restart();
}
static bool EstablishServerHandshakeBeforeReboot(Ota& ota) {
    std::unique_ptr<Protocol> protocol;
    if (ota.HasMqttConfig()) {
        protocol = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol config from OTA, skip pre-reboot server handshake");
        return false;
    }

    ESP_LOGI(TAG, "Starting pre-reboot server handshake");
    if (!protocol->Start()) {
        ESP_LOGE(TAG, "Pre-reboot protocol start failed");
        return false;
    }

    if (!protocol->OpenAudioChannel()) {
        ESP_LOGE(TAG, "Pre-reboot server hello failed");
        return false;
    }

    ESP_LOGI(TAG, "Pre-reboot server handshake completed, session_id=%s",
             protocol->session_id().c_str());
    protocol->CloseAudioChannel();
    return true;
}

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode() {
#ifdef CONFIG_USE_BLUFI_NET_CONFIGURING
    auto& application = Application::GetInstance();
    ESP_LOGI(TAG, "Entering WiFi config mode via BLUFI");
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, "请使用赛博星球小程序配网", "", Lang::Sounds::OGG_SCAN_BIND_CYBERPET);//暂时注释 有点吵
    
    uint8_t mac[6];
    static char blufi_device_name[20];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(blufi_device_name, sizeof(blufi_device_name), "CYBER_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "BLUFI device name: %s", blufi_device_name);

    char qr_url[128];
    snprintf(qr_url, sizeof(qr_url), "https://xiaozhi.cyberai.top/deviceBind/?m=%02X%02X%02X%02X%02X%02X&p=%s&n=%s&c=n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], BOARD_NAME, GetBoardType().c_str());

    auto display = Board::GetInstance().GetDisplay();
     if (display) {
         ESP_LOGI(TAG, "Calling display->ShowQRCode for %s", qr_url);
         display->ShowQRCode(qr_url, blufi_device_name);
     }

     vTaskDelay(pdMS_TO_TICKS(2000));
     bool is_got_ip = false;
     esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
         bool *is_got_ip = (bool *)arg;
         *is_got_ip = true;
         auto got_ip = static_cast<ip_event_got_ip_t*>(event_data);
         ESP_LOGI(TAG, "Got IP: " IPSTR ", netmask: " IPSTR ", gw: " IPSTR,
                  IP2STR(&got_ip->ip_info.ip), IP2STR(&got_ip->ip_info.netmask), IP2STR(&got_ip->ip_info.gw));
     }, &is_got_ip);
     
    // 用于控制配网流程的状态变量
    static bool wifi_config_completed = false;
    static bool ota_check_completed = false;
    wifi_config_completed = false;
    ota_check_completed = false;
     
     blufi_wificfg_cbs_t cbs = {
        .sta_config_cb = [](const wifi_config_t *config, void *arg) {
            ESP_LOGI(TAG, "Received sta config, ssid: %s, password_len: %d", config->sta.ssid, (int)strlen((const char*)config->sta.password));
            
            std::string ssid(reinterpret_cast<const char*>(config->sta.ssid));
            std::string password(reinterpret_cast<const char*>(config->sta.password));
            SsidManager::GetInstance().AddSsid(ssid, password);
            ESP_LOGI(TAG, "SSID stored. Total known SSIDs: %d", (int)SsidManager::GetInstance().GetSsidList().size());
        },
        .custom_data_cb = [](const uint8_t *data, size_t len, void *arg) {
            ESP_LOGI(TAG, "Received custom data (len=%d): %.*s", (int)len, (int)len, data);
            if (strncmp((char *)data, "AT+OTA=", 7) == 0) {
                std::string url(reinterpret_cast<const char*>(data+7), len-7);
                auto trim = [](std::string& s) {
                    const char* ws = " \t\r\n`\"'";
                    size_t start = s.find_first_not_of(ws);
                    if (start == std::string::npos) { s.clear(); return; }
                    size_t end = s.find_last_not_of(ws);
                    s = s.substr(start, end - start + 1);
                };
                trim(url);
                ESP_LOGI(TAG, "ota_url: %s", url.c_str());
                Settings settings("wifi", true);
                if (!url.empty()) {
                    settings.SetString("ota_url", url);
                }
            } else if (strncmp((char *)data, "ERROR:", 6) == 0) {
                // 处理从BLUFI层传来的错误消息
                ESP_LOGE(TAG, "BLUFI error: %.*s", (int)(len-6), data+6);
            } else {
                ESP_LOGW(TAG, "Unknown custom data, ignored");
            }
        },
         .error_cb = [](blufi_wificfg_error_t error, const char *message, void *arg) {
            ESP_LOGE(TAG, "BLUFI error callback: error=%d, message=%s", error, message ? message : "NULL");
            // Send error code to app via BLUFI (unified format similar to OTA_CHECK_TIMEOUT)
            if (blufi_wificfg_is_ble_connected()) {
                char error_msg[128];
                switch (error) {
                    case BLUFI_ERROR_WIFI_PASSWORD_WRONG:
                        snprintf(error_msg, sizeof(error_msg), "WIFI_AUTH_FAILED");
                        break;
                    case BLUFI_ERROR_WIFI_NETWORK_UNAVAILABLE:
                        snprintf(error_msg, sizeof(error_msg), "WIFI_NETWORK_UNAVAILABLE");
                        break;
                    case BLUFI_ERROR_WIFI_CONNECTION_TIMEOUT:
                        snprintf(error_msg, sizeof(error_msg), "WIFI_CONNECTION_TIMEOUT");
                        break;
                    case BLUFI_ERROR_BLE_DISCONNECTED:
                        snprintf(error_msg, sizeof(error_msg), "BLE_DISCONNECTED");
                        break;
                    default:
                        snprintf(error_msg, sizeof(error_msg), "WIFI_CONFIG_FAILED");
                        break;
                }
                blufi_wificfg_send_error_message(error_msg);
            }
        }
    };
    
    blufi_wificfg_start(true, blufi_device_name, cbs, this);
    ESP_LOGI(TAG, "BLUFI service started, waiting for STA IP...");
    
    // 等待获取IP，增加超时机制
    const int IP_WAIT_TIMEOUT_MS = 60000; // 60秒超时
    int64_t ip_wait_start = esp_timer_get_time() / 1000;

    while (!is_got_ip) {
        // 检查BLE连接状态，仅用于调试，不应频繁打印警告
        if (!blufi_wificfg_is_ble_connected()) {
            ESP_LOGD(TAG, "Waiting for BLE connection or IP...");
        }

        // 检查超时
        int64_t elapsed = (esp_timer_get_time() / 1000) - ip_wait_start;
        if (elapsed > IP_WAIT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "Timeout waiting for IP address");
            if (blufi_wificfg_is_ble_connected()) {
                blufi_wificfg_send_error_message("WIFI_IP_TIMEOUT");
            }
            // 不立即退出，允许继续等待或重试
            ip_wait_start = esp_timer_get_time() / 1000; // 重置超时计时
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGD(TAG, "Waiting for IP via BLUFI STA connection...");
    }

    wifi_config_completed = true;
    
    // 通知手机配网成功
    if (blufi_wificfg_is_ble_connected()) {
        blufi_wificfg_send_custom((uint8_t *)"WIFI_CONNECTED", strlen("WIFI_CONNECTED"));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "WiFi configuration completed. Device will reboot to enter normal mode and perform OTA check.");
    ESP_LOGI(TAG, "WiFi configuration completed, starting OTA check while BLE is still connected...");
    Ota ota;
    const int MAX_RETRY = 5;
    int retry_count = 0;
    int retry_delay = 5; // 初始重试延迟为5秒
    const int OTA_CHECK_TIMEOUT_MS = 60000; // OTA检查总超时60秒
    int64_t ota_check_start = esp_timer_get_time() / 1000;
    bool ota_success = false;

    while (true) {
        // 检查总超时
        int64_t elapsed = (esp_timer_get_time() / 1000) - ota_check_start;
        if (elapsed > OTA_CHECK_TIMEOUT_MS) {
            ESP_LOGE(TAG, "OTA check total timeout after %lld ms", elapsed);
            if (blufi_wificfg_is_ble_connected()) {
                blufi_wificfg_send_error_message("OTA_CHECK_TIMEOUT");
                blufi_wificfg_send_custom((uint8_t *)"OTA_CHECK_TIMEOUT", strlen("OTA_CHECK_TIMEOUT"));
            }
            break;
        }
        
        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                if (blufi_wificfg_is_ble_connected()) {
                    blufi_wificfg_send_error_message("OTA_CHECK_FAILED_TOO_MANY_RETRIES");
                    blufi_wificfg_send_custom((uint8_t *)"OTA_CHECK_FAILED", strlen("OTA_CHECK_FAILED"));
                }
                break;
            }
            
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            if (blufi_wificfg_is_ble_connected()) {
                char retry_msg[64];
                snprintf(retry_msg, sizeof(retry_msg), "OTA_CHECK_RETRY:%d/%d", retry_count, MAX_RETRY);
                blufi_wificfg_send_custom((uint8_t *)retry_msg, strlen(retry_msg));
            }
            
            vTaskDelay(pdMS_TO_TICKS(retry_delay * 1000));
            retry_delay = (retry_delay < 30) ? retry_delay * 2 : 30; // 最大延迟30秒
            continue;
        }

        ESP_LOGI(TAG, "OTA check success");
        ota_check_completed = true;
        ota_success = true;
        
        // 如果获取到版本号信息，则视为联网成功
        const std::string firmware_version = ota.GetFirmwareVersion();
        if (!firmware_version.empty()) {
            if (blufi_wificfg_is_ble_connected()) {
                blufi_wificfg_send_custom((uint8_t *)"OTA_CHECK_SUCCESS", strlen("OTA_CHECK_SUCCESS"));
            }
            ESP_LOGI(TAG, "Firmware version \"%s\" received", firmware_version.c_str());
        } else {
            if (blufi_wificfg_is_ble_connected()) {
                blufi_wificfg_send_custom((uint8_t *)"OTA_CHECK_FAILED", strlen("OTA_CHECK_FAILED"));
            }
            ESP_LOGI(TAG, "Firmware version missing, treating as OTA check failed");
        }
        break; // 跳出循环
    }
    if (ota_success) {
        bool server_ready = EstablishServerHandshakeBeforeReboot(ota);
        if (blufi_wificfg_is_ble_connected()) {
            const char* status = server_ready ? "SERVER_CONNECT_SUCCESS" : "SERVER_CONNECT_FAILED";
            blufi_wificfg_send_custom((uint8_t*)status, strlen(status));
        }
    }

    // 延迟一小段时间确保小程序收到 WIFI_CONNECTED 消息
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (display) {
        display->ShowSuccessScreen("WiFi 配置成功，设备即将重启...");
    }

    ESP_LOGI(TAG, "Scheduling reboot in 3s and stopping BLUFI...");
    esp_timer_handle_t reboot_timer;
    const esp_timer_create_args_t reboot_args = {
        .callback = &reboot_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reboot"
    };
    esp_timer_create(&reboot_args, &reboot_timer);
    esp_timer_start_once(reboot_timer, 3000000);
    blufi_wificfg_stop();
    vTaskDelay(pdMS_TO_TICKS(3000));
#else
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("Xiaozhi");
    wifi_ap.Start();

    // 等待 1.5 秒，确保 AP 启动完成
    vTaskDelay(pdMS_TO_TICKS(1500));

    // 显示 WiFi 配置 AP 的 SSID 和 Web 服务器 URL
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_ap.GetSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_ap.GetWebServerUrl();
    
    // 通知 WiFi 配置提示用户连接到 AP 并访问 Web 服务器
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);

    #if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = 1;
    if (codec) {
        channel = codec->input_channels();
    }
    ESP_LOGI(TAG, "Start receiving WiFi credentials from audio, input channels: %d", channel);
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap, display, channel);
    #endif
    
    // Wait forever until reset after configuration
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * Return device status JSON
     * 
     * The returned JSON structure is as follows:
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
