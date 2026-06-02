#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error != 0 ? last_error : ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
         return status_code;
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    // 在获取到配置信息后，连接到新的升级服务器检查固件升级
    if (has_mqtt_config_ || has_websocket_config_) {
        ESP_LOGI(TAG, "Configuration received, checking for firmware upgrades from upgrade server");
        std::string upgrade_server_url = GetUpgradeServerUrl();
        if (CheckUpgradeFromServer(upgrade_server_url)) {
            ESP_LOGI(TAG, "Successfully checked upgrade server for firmware updates");
        } else {
            ESP_LOGW(TAG, "Failed to check upgrade server, continuing with original OTA result");
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s at offset 0x%lx", partition->label, partition->address);
    
    // 检查启动分区
    const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
    if (boot_partition) {
        ESP_LOGI(TAG, "Boot partition: %s at offset 0x%lx", boot_partition->label, boot_partition->address);
        if (strcmp(partition->label, boot_partition->label) != 0) {
            ESP_LOGW(TAG, "Running partition differs from boot partition! Running: %s, Boot: %s",
                     partition->label, boot_partition->label);
        }
    }
    
    // 检查下一个更新分区
    const esp_partition_t* next_update = esp_ota_get_next_update_partition(NULL);
    if (next_update) {
        ESP_LOGI(TAG, "Next update partition: %s at offset 0x%lx", next_update->label, next_update->address);
    }
    
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    const char* state_str = "UNKNOWN";
    switch (state) {
        case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
        case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
        case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
        case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
        case ESP_OTA_IMG_UNDEFINED: state_str = "UNDEFINED"; break;
    }
    ESP_LOGI(TAG, "Current partition state: %s (%d)", state_str, state);

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGI(TAG, "Partition not pending verify, no action needed");
    }
    
    // 读取otadata分区内容进行调试
    const esp_partition_t* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                               ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        uint8_t otadata_buf[32];
        esp_partition_read(otadata, 0, otadata_buf, sizeof(otadata_buf));
        ESP_LOGI(TAG, "OTA data partition content after boot (first 32 bytes):");
        ESP_LOG_BUFFER_HEX(TAG, otadata_buf, sizeof(otadata_buf));
    }
}

bool Ota::Upgrade(const std::string& firmware_url) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    
    // 获取当前运行分区信息
    auto running_partition = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Currently running partition: %s at offset 0x%lx", 
             running_partition->label, running_partition->address);
    
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx, size: %lu bytes", 
             update_partition->label, update_partition->address, update_partition->size);
    
    // 检查分区状态
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(update_partition, &ota_state) == ESP_OK) {
        const char* state_str = "UNKNOWN";
        switch (ota_state) {
            case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
            case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
            case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
            case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
            case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
            case ESP_OTA_IMG_UNDEFINED: state_str = "UNDEFINED"; break;
        }
        ESP_LOGI(TAG, "Update partition current state: %s (%d)", state_str, ota_state);
    }
    
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }
    
    ESP_LOGI(TAG, "Firmware size to download: %u bytes", content_length);
    
    // 固件下载开始，通知服务器固件下发开始
    NotifyFirmwareDeliveryStart();

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    static size_t written = 0;
    // Track last logged progress percentage (0-100)
    int last_logged_percent = -1;
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false; 
        }
        if (written + ret > update_partition->size) {
            ESP_LOGE(TAG, "OTA bin too large for partition: %u > %u",
                    written + ret, update_partition->size);
            esp_ota_abort(update_handle);
            return false;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (!image_header_checked) {
            image_header.append(buffer, ret);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                // 打印固件头部的前64字节用于调试
                ESP_LOGI(TAG, "Firmware header (first 64 bytes):");
                ESP_LOG_BUFFER_HEX(TAG, image_header.data(), 64);
                
                // 检查是否是压缩固件 (TYPE_2 格式)
                // 压缩固件的魔数在偏移 0x120 处为 0x43 (compressed OTA header magic)
                esp_image_header_t* img_header = (esp_image_header_t*)image_header.data();
                ESP_LOGI(TAG, "Image header magic: 0x%02X, segment_count: %d, spi_mode: %d", 
                         img_header->magic, img_header->segment_count, img_header->spi_mode);
                
                // 检查是否是压缩OTA格式
                bool is_compressed = false;
                if (image_header.size() >= 0x120 + 8) {
                    uint8_t* compressed_header = (uint8_t*)image_header.data() + 0x120;
                    ESP_LOGI(TAG, "Checking compressed OTA header at offset 0x120:");
                    ESP_LOG_BUFFER_HEX(TAG, compressed_header, 40);
                    
                    // 压缩OTA头部的魔数
                    uint32_t compressed_magic = *(uint32_t*)compressed_header;
                    ESP_LOGI(TAG, "Compressed OTA magic: 0x%08lX", compressed_magic);
                    
                    // TYPE_2 格式的魔数是 0x43 ('C')
                    if (compressed_header[0] == 0x43) {
                        is_compressed = true;
                        ESP_LOGW(TAG, "*** COMPRESSED FIRMWARE DETECTED (TYPE_2 format) ***");
                        ESP_LOGI(TAG, "Bootloader should decompress this firmware during boot");
                    }
                }
                
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version from header: %s", new_app_info.version);
                ESP_LOGI(TAG, "App desc magic: 0x%08lX (expected 0x%08lX)", 
                         new_app_info.magic_word, (uint32_t)ESP_APP_DESC_MAGIC_WORD);

                auto current_version = esp_app_get_description()->version;
                if (!is_compressed && memcmp(new_app_info.version, current_version, sizeof(new_app_info.version)) == 0) {
                    ESP_LOGE(TAG, "Firmware version is the same, skipping upgrade");
                    return false;
                }
                ESP_LOGI(TAG, "Current version: %s, New version: %s, Is compressed: %s", 
                         current_version, new_app_info.version, is_compressed ? "YES" : "NO");

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    return false;
                }
                
                ESP_LOGI(TAG, "OTA begin successful, update_handle: %lu", (unsigned long)update_handle);

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }
        

        auto err = esp_ota_write(update_handle, buffer, ret);
        written += ret;

        // Calculate percent written relative to partition size (if known)
        if (update_partition->size > 0) {
            int percent = (int)((written * 100) / update_partition->size);
            // Log only when percent crosses a 10% boundary or at 100%
            if (percent >= 100) percent = 100;
            if (percent % 10 == 0 && percent != last_logged_percent) {
                ESP_LOGI(TAG, "update_partition size: %u, written: %u (%d%%)", update_partition->size, written, percent);
                last_logged_percent = percent;
            }
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return false;
        }
    }
    http->Close();

    // 固件下载完成，通知服务器固件下发成功
    NotifyFirmwareDeliveryComplete(true, content_length);


    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        // 通知服务器升级失败
        NotifyUpgradeComplete(false);
        return false;
    }
    
    ESP_LOGI(TAG, "esp_ota_end() successful, firmware written to partition");
    
    // 检查写入后的分区状态
    esp_ota_img_states_t post_write_state;
    if (esp_ota_get_state_partition(update_partition, &post_write_state) == ESP_OK) {
        const char* state_str = "UNKNOWN";
        switch (post_write_state) {
            case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
            case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
            case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
            case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
            case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
            case ESP_OTA_IMG_UNDEFINED: state_str = "UNDEFINED"; break;
        }
        ESP_LOGI(TAG, "Partition state after write: %s (%d)", state_str, post_write_state);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        // 通知服务器升级失败
        NotifyUpgradeComplete(false);
        return false;
    }
    
    ESP_LOGI(TAG, "Boot partition set to: %s at offset 0x%lx", 
             update_partition->label, update_partition->address);
    
    // 验证启动分区设置
    const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
    if (boot_partition) {
        ESP_LOGI(TAG, "Verified boot partition: %s at offset 0x%lx", 
                 boot_partition->label, boot_partition->address);
        if (strcmp(boot_partition->label, update_partition->label) != 0) {
            ESP_LOGE(TAG, "WARNING: Boot partition mismatch! Expected %s but got %s", 
                     update_partition->label, boot_partition->label);
        }
    } else {
        ESP_LOGE(TAG, "Failed to get boot partition after setting!");
    }
    
    // 读取otadata分区内容进行调试
    const esp_partition_t* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                               ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        uint8_t otadata_buf[32];
        esp_partition_read(otadata, 0, otadata_buf, sizeof(otadata_buf));
        ESP_LOGI(TAG, "OTA data partition content (first 32 bytes):");
        ESP_LOG_BUFFER_HEX(TAG, otadata_buf, sizeof(otadata_buf));
    }

    ESP_LOGI(TAG, "Firmware upgrade successful, next boot will be from %s", update_partition->label);
    // 通知服务器升级完成
    NotifyUpgradeComplete(true);
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    // 如果有固件URL，直接升级
    if (!firmware_url_.empty()) {
        return Upgrade(firmware_url_);
    }
    
    // 如果没有固件URL，尝试从升级服务器获取
    ESP_LOGW(TAG, "No firmware URL available, trying to get from upgrade server");
    std::string upgrade_server_url = GetUpgradeServerUrl();
    if (CheckUpgradeFromServer(upgrade_server_url) && has_new_version_) {
        ESP_LOGI(TAG, "Got firmware URL from upgrade server: %s", firmware_url_.c_str());
        return Upgrade(firmware_url_);
    }
    
    ESP_LOGE(TAG, "No firmware URL available for upgrade");
    return false;
}

bool Ota::StartUpgradeFromUrl(const std::string& url, std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return Upgrade(url);
}

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}

// 新增方法实现
std::string Ota::GetUpgradeServerUrl() {
    // 返回固定的升级服务器地址
    return "http://ota.cyberai.top";// http://192.168.50.27:8000
}

std::string Ota::GetDeviceInfoJson() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "board_name", BOARD_NAME);
    cJSON_AddStringToObject(root, "current_version", app_desc->version);
    cJSON_AddStringToObject(root, "uuid", board.GetUuid().c_str());
    cJSON_AddStringToObject(root, "chip_model", SystemInfo::GetChipModelName().c_str());
    cJSON_AddStringToObject(root, "flash_size", std::to_string(SystemInfo::GetFlashSize()).c_str());
    
    // 添加时间戳
    time_t now = time(nullptr);
    cJSON_AddNumberToObject(root, "timestamp", now);
    
    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return json;
}

bool Ota::CheckUpgradeFromServer(const std::string& upgrade_server_url) {
    ESP_LOGI(TAG, "Checking upgrade from server: %s", upgrade_server_url.c_str());
    
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();
    current_version_ = app_desc->version;
    
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", std::string(BOARD_NAME "/") + app_desc->version);
    
    std::string device_info = GetDeviceInfoJson();
    http->SetContent(std::move(device_info));
    
    std::string url = upgrade_server_url + "/check";
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to upgrade server");
        return false;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check upgrade, status code: %d", status_code);
        return false;
    }
    
    std::string response = http->ReadAll();
    http->Close();
    
    cJSON *root = cJSON_Parse(response.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response from upgrade server");
        return false;
    }
    
    has_new_version_ = false;
    cJSON *upgrade = cJSON_GetObjectItem(root, "upgrade");
    if (cJSON_IsObject(upgrade)) {
        cJSON *available = cJSON_GetObjectItem(upgrade, "available");
        cJSON *version = cJSON_GetObjectItem(upgrade, "version");
        cJSON *url = cJSON_GetObjectItem(upgrade, "url");
        cJSON *force = cJSON_GetObjectItem(upgrade, "force");
        
        ESP_LOGI(TAG, "Upgrade server response - available: %s, version: %s, url: %s, force: %s", 
                 cJSON_IsBool(available) ? (available->valueint ? "true" : "false") : "null",
                 cJSON_IsString(version) ? version->valuestring : "null",
                 cJSON_IsString(url) ? url->valuestring : "null",
                 cJSON_IsBool(force) ? (force->valueint ? "true" : "false") : "null");
        
        if (cJSON_IsBool(available) && available->valueint) {
            if (cJSON_IsString(version)) {
                firmware_version_ = version->valuestring;
            }
            if (cJSON_IsString(url)) {
                firmware_url_ = url->valuestring;
            }
            
            // 检查是否有强制升级标志
            bool force_upgrade = false;
            if (cJSON_IsBool(force)) {
                force_upgrade = force->valueint;
            }
            
            if (cJSON_IsString(version) && cJSON_IsString(url)) {
                if (force_upgrade) {
                    has_new_version_ = true;
                    ESP_LOGI(TAG, "Force upgrade to version: %s", firmware_version_.c_str());
                } else {
                    has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_.c_str());
                    ESP_LOGI(TAG, "Version comparison - current: %s, target: %s, has_new: %s", 
                             current_version_.c_str(), firmware_version_.c_str(), has_new_version_ ? "true" : "false");
                    if (has_new_version_) {
                        ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
                    } else {
                        ESP_LOGI(TAG, "Current is the latest version");
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Upgrade not available from server");
        }
    }
    
    // 如果从升级服务器检测到新版本，覆盖原来的版本信息
    if (has_new_version_) {
        ESP_LOGI(TAG, "Upgrade server has newer version: %s, overriding original OTA result", firmware_version_.c_str());
    }
    
    // 检查是否需要上报升级成功状态
    // 如果当前版本与目标版本一致，说明升级成功
    if (!firmware_version_.empty() && current_version_ == firmware_version_) {
        ESP_LOGI(TAG, "Current version matches target version, upgrade may have been completed");
        // 只有在确实需要升级时才通知服务器
        // NotifyUpgradeComplete(true); // 注释掉这行，避免误报升级成功
    }
    
    cJSON_Delete(root);
    return true;
}

bool Ota::StartUpgradeFromServer(const std::string& upgrade_server_url,
                                 std::function<void(int progress, size_t speed)> callback,
                                 bool skip_check) {
    ESP_LOGI(TAG, "Starting upgrade from server: %s", upgrade_server_url.c_str());
    
    if (!skip_check) {
        // 首先检查是否有可用升级
        if (!CheckUpgradeFromServer(upgrade_server_url)) {
            ESP_LOGE(TAG, "Failed to check upgrade from server");
            return false;
        }
    }

    if (!has_new_version_) {
        ESP_LOGI(TAG, "No new version available from upgrade server");
        return false;
    }

    if (firmware_url_.empty()) {
        ESP_LOGE(TAG, "Firmware URL is empty after checking upgrade server");
        return false;
    }

    // 记录升级请求到服务器
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(request, "board_name", BOARD_NAME);
    cJSON_AddStringToObject(request, "current_version", current_version_.c_str());
    cJSON_AddStringToObject(request, "target_version", firmware_version_.c_str());
    cJSON_AddStringToObject(request, "action", "start_upgrade");
    
    auto request_str = cJSON_PrintUnformatted(request);
    http->SetContent(std::string(request_str));
    cJSON_free(request_str);
    cJSON_Delete(request);
    
    std::string url = upgrade_server_url + "/upgrade_request";
    if (http->Open("POST", url)) {
        ESP_LOGI(TAG, "Upgrade request sent to server");
    }
    http->Close();
    
    // 开始升级
    upgrade_callback_ = callback;
    return Upgrade(firmware_url_);
}

void Ota::NotifyUpgradeComplete(bool success) {
    ESP_LOGI(TAG, "Notifying upgrade server of completion, success: %s", success ? "true" : "false");
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    
    cJSON *notification = cJSON_CreateObject();
    cJSON_AddStringToObject(notification, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(notification, "board_name", BOARD_NAME);
    cJSON_AddStringToObject(notification, "current_version", current_version_.c_str());
    cJSON_AddStringToObject(notification, "target_version", firmware_version_.c_str());
    cJSON_AddBoolToObject(notification, "success", success);
    
    auto notification_str = cJSON_PrintUnformatted(notification);
    http->SetContent(std::string(notification_str));
    cJSON_free(notification_str);
    cJSON_Delete(notification);
    
    std::string url = GetUpgradeServerUrl() + "/api/notify_upgrade_complete";
    if (http->Open("POST", url)) {
        auto status_code = http->GetStatusCode();
        if (status_code == 200) {
            ESP_LOGI(TAG, "Upgrade completion notification sent successfully");
        } else {
            ESP_LOGE(TAG, "Failed to notify upgrade completion, status code: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Failed to send upgrade completion notification");
    }
    http->Close();
}

void Ota::NotifyFirmwareDeliveryStart() {
    ESP_LOGI(TAG, "Notifying upgrade server of firmware delivery start");
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    
    cJSON *notification = cJSON_CreateObject();
    cJSON_AddStringToObject(notification, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(notification, "board_name", BOARD_NAME);
    cJSON_AddStringToObject(notification, "current_version", current_version_.c_str());
    cJSON_AddStringToObject(notification, "target_version", firmware_version_.c_str());
    cJSON_AddStringToObject(notification, "status", "delivery_started");
    
    auto notification_str = cJSON_PrintUnformatted(notification);
    http->SetContent(std::string(notification_str));
    cJSON_free(notification_str);
    cJSON_Delete(notification);
    
    std::string url = GetUpgradeServerUrl() + "/api/update_firmware_delivery";
    if (http->Open("POST", url)) {
        auto status_code = http->GetStatusCode();
        if (status_code == 200) {
            ESP_LOGI(TAG, "Firmware delivery start notification sent successfully");
        } else {
            ESP_LOGE(TAG, "Failed to notify firmware delivery start, status code: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Failed to send firmware delivery start notification");
    }
    http->Close();
}

void Ota::NotifyFirmwareDeliveryComplete(bool success, size_t firmware_size) {
    ESP_LOGI(TAG, "Notifying upgrade server of firmware delivery complete, success: %s, size: %u", success ? "true" : "false", (unsigned int)firmware_size);
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    
    cJSON *notification = cJSON_CreateObject();
    cJSON_AddStringToObject(notification, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(notification, "target_version", firmware_version_.c_str());
    cJSON_AddBoolToObject(notification, "success", success);
    cJSON_AddNumberToObject(notification, "firmware_size", firmware_size);
    
    auto notification_str = cJSON_PrintUnformatted(notification);
    http->SetContent(std::string(notification_str));
    cJSON_free(notification_str);
    cJSON_Delete(notification);
    
    std::string url = GetUpgradeServerUrl() + "/api/update_firmware_delivery";
    if (http->Open("POST", url)) {
        auto status_code = http->GetStatusCode();
        if (status_code == 200) {
            ESP_LOGI(TAG, "Firmware delivery complete notification sent successfully");
        } else {
            ESP_LOGE(TAG, "Failed to notify firmware delivery complete, status code: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Failed to send firmware delivery complete notification");
    }
    http->Close();
}
