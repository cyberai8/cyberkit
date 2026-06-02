#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <thread>   // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "lvgl_display.h"
#define TAG "Esp32Music"

//#define LV_COLOR_FORMAT_RGB565 0x12
//#define LV_COLOR_FORMAT_RGB565A8 0x0A


// ========== 简单的ESP32认证函数 ==========

static cJSON* select_song_by_artist(cJSON* response_json, const std::string& artist_name);
/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id() {
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "your-esp32-secret-key-2024";
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http* http) {
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000;  // 转换为秒
    
    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 添加认证头
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        //ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", 
        //         mac.c_str(), chip_id.c_str(), timestamp);
    }
}

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建
static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        
        pos = amp_pos + 1;
    }
    
    // 处理最后一个参数
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    
    return result_url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false),network_recovery_in_progress_(false) {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
     // 设置默认线程配置
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 4096;  // 默认4KB栈大小
    cfg.prio = 3;
    cfg.thread_name = "music_player";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // 将堆栈分配到 PSRAM 中
    esp_pthread_set_cfg(&cfg);

    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
            ESP_LOGI(TAG, "Destroying music player");
        
        // 停止所有操作
        is_downloading_ = false;
        is_playing_ = false;
        is_lyric_running_ = false;
        
        // 通知所有等待的线程
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // 等待线程结束
        WaitForThreadsToFinish();
        
        // 安全清理内存
        SafeMemoryCleanup();
        
        ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    // 使用超时保护的下载函数
    return DownloadWithTimeout(song_name, artist_name, 10000); // 10秒超时
}

bool Esp32Music::DownloadWithTimeout(const std::string& song_name, const std::string& artist_name, int timeout_ms) {
    std::atomic<bool> download_completed(false);
    std::atomic<bool> download_success(false);
    
    // 设置线程栈大小以避免在获取音乐链接时堆栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 12288; // 恢复到 12KB 以避免 Stack Overflow
    cfg.thread_name = "music_download";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // 将堆栈分配到 PSRAM 中
    esp_pthread_set_cfg(&cfg);

    // 在新线程中执行下载
    std::thread download_thread([&]() {
        download_success = DownloadInternal(song_name, artist_name);
        download_completed.store(true);
    });
    
    download_thread.detach();
    
    // 等待超时
    auto start_time = std::chrono::steady_clock::now();
    while (!download_completed.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        
        if (elapsed.count() >= timeout_ms) {
            ESP_LOGE(TAG, "Download timeout after %d ms for song: %s", timeout_ms, song_name.c_str());
            
            // 在这里可以尝试取消HTTP请求（如果支持）
            // 注意：这可能无法完全停止底层的网络操作
            
            return false;
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    return download_success.load();
}
// bool Esp32Music::DownloadInternal(const std::string& song_name, const std::string& artist_name) {
//     // 这里放置原来的Download函数的主体代码
//     // 但去掉最外层的函数签名
//     // ... [原来的Download函数代码] ...
    
//     // 注意：这个函数应该是原来Download函数的拷贝，只是改了个名字
// }
bool Esp32Music::DownloadInternal(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();
    
    // 保存歌名用于后续显示
    current_song_name_ = song_name;
   // 第一步：请求stream_pcm接口获取音频信息
    std::string base_url = "暂无API";
    std::string full_url_name = base_url + "&name=" + url_encode(song_name);
    std::string platform_1 = "&type=kg";//酷狗 默认
    std::string platform_2 = "&type=wyy";//网易云
    std::string platform_3 = "&type=qq";//QQ音乐
    std::string platform_4 = "&type=kw";//酷我
    std::string platform_5 = "&type=mg";//咪咕
    std::string full_url_artist = base_url + "&singer=" + url_encode(artist_name);
    std::string shykey_text = "暂无API";
    std::string full_url;
    if(!song_name.empty()) {
        full_url = full_url_name;
        ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    } else {
        full_url = full_url_artist;
        ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    }
    
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    // 使用Board提供的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    
    // 添加ESP32认证头
    add_auth_headers(http.get());
    
    // 打开GET连接
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }
    
    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }
    ESP_LOGI(TAG,"开始读取响应数据...");
    // 读取响应数据
    last_downloaded_data_ = http->ReadAll();
    http->Close();
    
    //ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, last_downloaded_data_.length());
    //ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    // 简单的认证响应检查（可选）
    if (last_downloaded_data_.find("ESP32动态密钥验证失败") != std::string::npos) {
        ESP_LOGE(TAG, "Authentication failed for song: %s", song_name.c_str());
        return false;
    }
    
    if (!last_downloaded_data_.empty()) {
        // 解析响应JSON以提取音频URL
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            // 提取关键信息
            ESP_LOGI(TAG,"开始解析音乐信息...");
             cJSON* selected_item = select_song_by_artist(response_json, artist_name);
            // 字段映射
            ESP_LOGI(TAG,"完成解析音乐信息");
            cJSON *artist = cJSON_GetObjectItem(selected_item, "singer");        // 歌手信息
            cJSON *title = cJSON_GetObjectItem(selected_item, "name");           // 歌曲名称
            cJSON *audio_url = cJSON_GetObjectItem(selected_item, "url");        // 音频URL
            //cJSON *lyric_url = cJSON_GetObjectItem(selected_item, "url_lrc");    // 歌词URL
            //cJSON *img_url = cJSON_GetObjectItem(selected_item, "pic");          // 封面图片URL
            //cJSON *duration = cJSON_GetObjectItem(selected_item, "duration");    // 时长(秒)
            
            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Artist: %s", artist->valuestring);
                current_artist_name_ = artist->valuestring;
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Title: %s", title->valuestring);
            }
            // 获取音乐总时长
            // if (cJSON_IsNumber(duration)) {
            //     total_duration_ms_ = duration->valueint; // 秒转毫秒
            //     ESP_LOGI(TAG, "Music total duration: %d seconds (%d ms)", 
            //             duration->valueint, total_duration_ms_*1000);
            // } else {
            //     // 如果没有提供时长，使用默认值或后续估算
            //     total_duration_ms_ = 0;
            //     //ESP_LOGW(TAG, "No duration information in response, will estimate from stream");
            // }
            // 检查audio_url是否有效
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                ESP_LOGI(TAG, "Audio URL path: %s", audio_url->valuestring);
                
                // 第二步：拼接完整的音频下载URL，确保对audio_url进行URL编码
                std::string audio_path = audio_url->valuestring;

                // 使用字符串流安全构建URL
                std::stringstream url_builder;
                url_builder << audio_path;

                // 添加分隔符
                if (audio_path.find('?') == std::string::npos) {
                    url_builder << "?";
                } else {
                    url_builder << "&";
                }

                url_builder << shykey_text;

                current_music_url_ = url_builder.str();

                ESP_LOGI(TAG, "Final Audio URL: %s", current_music_url_.c_str());
                ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
                song_name_displayed_ = false;  // 重置歌名显示标志
                StartStreaming(current_music_url_);
                
                // 处理歌词URL - 只有在歌词显示模式下才启动歌词
                // if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                //     // 拼接完整的歌词下载URL，使用相同的URL构建逻辑
                //     std::string lyric_path = lyric_url->valuestring;
                    
                //     current_lyric_url_ = lyric_path + "&"+shykey_text;
                //     // 根据显示模式决定是否启动歌词
                //     if (display_mode_ == DISPLAY_MODE_LYRICS) {
                //         ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());
                        
                //         // 启动歌词下载和显示
                //         if (is_lyric_running_) {
                //             is_lyric_running_ = false;
                //             if (lyric_thread_.joinable()) {
                //                 lyric_thread_.join();
                //             }
                //         }
                        
                //         is_lyric_running_ = true;
                //         current_lyric_index_ = -1;
                //         lyrics_.clear();
                //         // 在创建新歌词线程前
                //         if (lyric_thread_.joinable()) {
                //             lyric_thread_.detach();
                //         }
                //         //lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                //     } else {
                //         ESP_LOGI(TAG, "Lyric URL found but spectrum display mode is active, skipping lyrics");
                //     }
                // } else {
                //     ESP_LOGW(TAG, "No lyric URL found for this song");
                // }
                 // 处理封面URL - 完全禁用或条件启用
                // if (cJSON_IsString(img_url) && img_url->valuestring && strlen(img_url->valuestring) > 0) {
                //     current_cover_url_ = img_url->valuestring;
                    
                //     // 条件启用封面下载
                //     #ifdef ENABLE_COVER_DOWNLOAD
                //         // 只在内存充足时启用
                //         size_t free_heap = esp_get_free_heap_size();
                //         if (free_heap > 300 * 1024 && display_mode_ == DISPLAY_MODE_LYRICS) {
                //             StartCoverDownloadDelayed();
                //         } else {
                //             ESP_LOGI(TAG, "Skipping cover due to memory constraints: %d bytes free", free_heap);
                //         }
                //     #else
                //         ESP_LOGI(TAG, "Cover download disabled at compile time");
                //     #endif
                // } else {
                //     ESP_LOGW(TAG, "No cover URL found for this song");
                // }
                
                cJSON_Delete(response_json);
                return true;
            } else {
                // audio_url为空或无效
                ESP_LOGE(TAG, "Audio URL not found or empty for song: %s", song_name.c_str());
                ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    
    return false;
}



std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;
    
    // 等待之前的线程完全结束
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        play_thread_.join();
    }
    
    // 清空缓冲区
    ClearAudioBuffer();
    
    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 12288;  // 恢复到较大栈大小 12KB 以避免 Stack Overflow
    cfg.prio = 3;           // 中等优先级
    cfg.thread_name = "audio_stream";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // 将堆栈分配到 PSRAM 中
    esp_pthread_set_cfg(&cfg);
    
    // 开始下载线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    vTaskDelay(pdMS_TO_TICKS(100));
    // 开始播放线程（会等待缓冲区有足够数据）
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

        // 立即设置停止标志
    is_downloading_ = false;
    is_playing_ = false;
    is_cover_running_ = false;
    is_lyric_running_ = false;


    // 强制通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待线程结束，确保线程完全退出释放堆栈
    WaitForThreadsToFinish();

    // 强制清空音频缓冲区
    ClearAudioBuffer();
    
    // 给音频系统一点时间处理完最后的残留数据
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 重置采样率到原始值 (一般是 16000Hz)
    ResetSampleRate();
    
    // 再次给 I2S 总线一点时间稳定，避免采样率切换瞬间影响录音
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 重新确保编解码器输入开启，以防其在切换过程中被关闭或进入低功耗
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && !codec->input_enabled()) {
        codec->EnableInput(true);
        ESP_LOGI(TAG, "恢复音频编解码器输入以支持聆听");
    }
    
    // 清空歌名显示
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display");
    }
    
    // 清理全局FFT数据指针
    if (final_pcm_data_fft) {
        heap_caps_free(final_pcm_data_fft);
        final_pcm_data_fft = nullptr;
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}
// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    // 添加断点续传相关变量
    size_t total_bytes_received = 0;
    size_t last_successful_position = 0;
    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 3;
    
    const int max_retries = 5;
    int retry_count = 0;
    bool download_success = false;
    std::string current_url = music_url;
    int redirect_count = 0;
    const int max_redirects = 5;
    
    // 状态变量用于跟踪网络状况
    enum DownloadState {
        STATE_NORMAL,
        STATE_RECOVERING,
        STATE_FAILED
    };
    
    DownloadState download_state = STATE_NORMAL;
    
    while (retry_count < max_retries && !download_success && is_downloading_ && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying audio download (attempt %d of %d), last position: %d bytes", 
                    retry_count + 1, max_retries, last_successful_position);
            vTaskDelay(pdMS_TO_TICKS(1000 * retry_count));
        }
        
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            retry_count++;
            continue;
        }
        
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "*/*");
        
        // 如果之前有成功下载过数据，尝试断点续传
        if (last_successful_position > 0 && download_state == STATE_RECOVERING) {
            std::string range_header = "bytes=" + std::to_string(last_successful_position) + "-";
            http->SetHeader("Range", range_header.c_str());
            ESP_LOGI(TAG, "Attempting resume from position: %d bytes", last_successful_position);
        } else {
            http->SetHeader("Range", "bytes=0-");
        }
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        ESP_LOGI(TAG, "Opening HTTP connection to: %s", current_url.c_str());
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to connect to music stream URL");
            retry_count++;
            download_state = STATE_FAILED;
            continue;
        }

        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Audio stream HTTP status: %d", status_code);
        
        // 处理重定向
        if (status_code == 301 || status_code == 302 || status_code == 303 || 
            status_code == 307 || status_code == 308) {
            
            std::string location = http->GetResponseHeader("Location");
            ESP_LOGI(TAG, "Redirect received, Location header: %s", location.c_str());
            
            http->Close();
            
            if (!location.empty()) {
                // 处理相对路径重定向
                if (location.find("http") != 0) {
                    size_t last_slash = current_url.find_last_of('/');
                    if (last_slash != std::string::npos) {
                        std::string base_url = current_url.substr(0, last_slash + 1);
                        if (location[0] == '/') {
                            size_t protocol_end = current_url.find("://");
                            if (protocol_end != std::string::npos) {
                                size_t domain_start = protocol_end + 3;
                                size_t domain_end = current_url.find('/', domain_start);
                                if (domain_end != std::string::npos) {
                                    base_url = current_url.substr(0, domain_end);
                                }
                            }
                            current_url = base_url + location;
                        } else {
                            current_url = base_url + location;
                        }
                    } else {
                        current_url = location;
                    }
                } else {
                    current_url = location;
                }
                
                redirect_count++;
                ESP_LOGI(TAG, "Following redirect to: %s (redirect %d of %d)", 
                         current_url.c_str(), redirect_count, max_redirects);
                continue;
            } else {
                ESP_LOGE(TAG, "Redirect received but no Location header found");
                retry_count++;
                continue;
            }
        }
        
        // 处理206 Partial Content（断点续传）
        if (status_code == 206) {
            ESP_LOGI(TAG, "Server accepted resume request (206 Partial Content)");
        }
        
        // 检查是否是成功的状态码
        if (status_code != 200 && status_code != 206) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            download_state = STATE_FAILED;
            continue;
        }
        
        ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
        
        // 分块读取音频数据
        const size_t chunk_size = 4096;
        char buffer[chunk_size];
        size_t session_bytes_received = 0;
        bool connection_ok = true;
        consecutive_failures = 0;

        while (is_downloading_ && is_playing_ && connection_ok) {
            int bytes_read = http->Read(buffer, chunk_size);
            
            if (bytes_read > 0) {
                // 成功读取数据，重置失败计数
                consecutive_failures = 0;
                session_bytes_received += bytes_read;
                total_bytes_received += bytes_read;
                last_successful_position = total_bytes_received;
                download_state = STATE_NORMAL;
                
                // 创建音频数据块
                uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
                if (!chunk_data) {
                    ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
                    connection_ok = false;
                    break;
                }
                memcpy(chunk_data, buffer, bytes_read);
                
                // 等待缓冲区有空间
                {
                    std::unique_lock<std::mutex> lock(buffer_mutex_);
                    
                    // if (buffer_size_ > MAX_BUFFER_SIZE * 0.8) {
                    //     // 缓冲区使用率超过80%，减缓下载速度
                    //     std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    // }

                    // 添加缓冲区管理策略
                    // if (buffer_size_ >= MAX_BUFFER_SIZE * 0.9) {
                    //     // 缓冲区接近满，暂停下载一小会儿让播放线程消耗
                    //     lock.unlock();
                    //     std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    //     lock.lock();
                    // }
                    
                    buffer_cv_.wait(lock, [this] { 
                        return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; 
                    });
                    
                    if (is_downloading_) {
                        audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                        buffer_size_ += bytes_read;
                        
                        // 通知播放线程有新数据
                        buffer_cv_.notify_one();
                        
                        // 定期打印进度，但不要太频繁
                        if (total_bytes_received % (512 * 1024) == 0) {
                            ESP_LOGI(TAG, "Downloaded %d bytes total, buffer size: %d", 
                                    total_bytes_received, buffer_size_);
                        }
                    } else {
                        heap_caps_free(chunk_data);
                        break;
                    }
                }
                
            } else if (bytes_read == 0) {
                // 正常结束
                ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", total_bytes_received);
                download_success = true;
                break;
            } else if (bytes_read == -1) {
                // HTTP错误-1
                ESP_LOGW(TAG, "HTTP read returned -1 (likely connection issue)");
                consecutive_failures++;
                
                // 检查是否连续失败次数过多
                if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                    ESP_LOGW(TAG, "Too many consecutive failures (%d), will try to resume", 
                            consecutive_failures);
                    
                    // 标记为恢复状态，下次重试时从当前位置续传
                    download_state = STATE_RECOVERING;
                    connection_ok = false;
                    break;
                }
                
                // 短暂延迟后重试读取
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            } else {
                // 其他错误
                ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
                connection_ok = false;
                break;
            }
        }
        
        http->Close();
        
        if (!download_success) {
            retry_count++;
            // 如果不是恢复状态，重置成功位置
            if (download_state != STATE_RECOVERING) {
                last_successful_position = 0;
            }
        }
    }
    
    if (!download_success) {
        ESP_LOGE(TAG, "Failed to download audio after %d attempts and %d redirects", 
                retry_count, redirect_count);
        Application::GetInstance().TriggerWakeWord("获取音乐失败");
    }

    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Audio stream download thread finished, total downloaded: %d bytes", 
            total_bytes_received);
}
// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
        // 使用智能指针或确保单次释放的内存管理
    // 播放状态管理
    enum PlaybackState {
        PLAYBACK_PLAYING,
        PLAYBACK_BUFFERING,
        PLAYBACK_PAUSED,
        PLAYBACK_FINISHED
    };
    
    PlaybackState playback_state = PLAYBACK_BUFFERING;
    int buffer_starvation_count = 0;
    const int MAX_BUFFER_STARVATION = 10;  // 最多等待10次

    int16_t* pcm_buffer = nullptr;
    uint8_t* mp3_input_buffer = nullptr;
    
    // 分配内存
    pcm_buffer = (int16_t*)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    
    if (!pcm_buffer || !mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate playback buffers");
        // 安全释放
        if (pcm_buffer) {
            heap_caps_free(pcm_buffer);
            pcm_buffer = nullptr;
        }
        if (mp3_input_buffer) {
            heap_caps_free(mp3_input_buffer);
            mp3_input_buffer = nullptr;
        }
        is_playing_ = false;
        return;
    }
    
   // 检查必要的组件
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        // 直接清理并返回
        if (pcm_buffer) {
            heap_caps_free(pcm_buffer);
            pcm_buffer = nullptr;
        }
        if (mp3_input_buffer) {
            heap_caps_free(mp3_input_buffer);
            mp3_input_buffer = nullptr;
        }
        is_playing_ = false;
        return;
    }
    
    if (!mp3_decoder_initialized_ || !mp3_decoder_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        // 直接清理并返回
        if (pcm_buffer) {
            heap_caps_free(pcm_buffer);
            pcm_buffer = nullptr;
        }
        if (mp3_input_buffer) {
            heap_caps_free(mp3_input_buffer);
            mp3_input_buffer = nullptr;
        }
        is_playing_ = false;
        return;
    }
     
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        // 使用超时等待，避免无限期阻塞
        auto wait_start_time = std::chrono::steady_clock::now();
        bool buffer_ready = buffer_cv_.wait_for(lock, std::chrono::seconds(5), [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || 
                   (!is_downloading_ && !audio_buffer_.empty()) ||
                   !is_playing_;
        });
        if (!buffer_ready) {
            ESP_LOGE(TAG, "Timeout waiting for initial buffer data");
            is_playing_ = false;
            // 清理内存...
            if (pcm_buffer) {
                heap_caps_free(pcm_buffer);
                pcm_buffer = nullptr;
            }
            if (mp3_input_buffer) {
                heap_caps_free(mp3_input_buffer);
                mp3_input_buffer = nullptr;
            }
            return;
        }
        // 再次检查是否应该继续
        if (!is_playing_) {
            ESP_LOGI(TAG, "Playback stopped before starting");
            // 清理内存
            if (pcm_buffer) {
                heap_caps_free(pcm_buffer);
                pcm_buffer = nullptr;
            }
            if (mp3_input_buffer) {
                heap_caps_free(mp3_input_buffer);
                mp3_input_buffer = nullptr;
            }
            return;
        }
    }
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区
    //mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    bool change_onece = false;
    while (is_playing_) {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            if (current_state == kDeviceStateSpeaking) {
                    
            }
            if (current_state == kDeviceStateListening) { 
                ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            }
            // 切换状态
            if(!change_onece) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                change_onece = true;
            }
            app.SwitchToIdle(); // 变成待机状态
            ESP_LOGI(TAG, "Switched device to idle state for music playback");
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                // 格式化歌名显示为《歌名》播放中...
                std::string formatted_song_name = "《" + current_song_name_ + "》 " + current_artist_name_;
                display->SetChatMessage("lyric", formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    display->start();
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                } else {
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
            }
        }
        // 检查缓冲区状态
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (audio_buffer_.empty() && buffer_size_ == 0 && !is_downloading_) {
                // 下载完成且缓冲区为空，播放结束
                ESP_LOGI(TAG, "Playback finished normally");
                playback_state = PLAYBACK_FINISHED;
                break;
            }
            
            // 检查缓冲区是否饥饿
            if (audio_buffer_.empty() || buffer_size_ < MIN_BUFFER_SIZE / 2) {
                if (playback_state == PLAYBACK_PLAYING) {
                    playback_state = PLAYBACK_BUFFERING;
                    buffer_starvation_count++;
                    ESP_LOGW(TAG, "Buffer starvation detected, count: %d, buffer size: %d", 
                            buffer_starvation_count, buffer_size_);
                }
            } else {
                playback_state = PLAYBACK_PLAYING;
                buffer_starvation_count = 0;
            }
            
            // 如果缓冲区饥饿次数过多，可能需要采取措施
            if (buffer_starvation_count >= MAX_BUFFER_STARVATION) {
                ESP_LOGW(TAG, "Too many buffer starvations, checking download state");
                if (is_downloading_) {
                    // 下载仍在进行，但数据没到达，可能是网络问题
                    ESP_LOGW(TAG, "Download is active but buffer is starving, waiting...");
                }
            }
        }

        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            bool got_chunk = false;
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        playback_state = PLAYBACK_FINISHED;
                        break;
                    }
                     // 使用超时等待，避免无限期阻塞
                    auto wait_result = buffer_cv_.wait_for(lock, std::chrono::seconds(2));
                    
                    if (wait_result == std::cv_status::timeout) {
                        ESP_LOGW(TAG, "Timeout waiting for audio data");
                        // 检查是否应该继续
                        if (!is_downloading_) {
                            ESP_LOGI(TAG, "Download stopped, ending playback");
                            break;
                        }
                        // 继续等待下一轮
                        continue;
                    }
                    
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                got_chunk = true;
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (got_chunk && chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // 使用Application默认的帧时长
                packet.timestamp = 0;
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // 打印播放进度
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            
            // 跳过一些字节继续尝试
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
            }
        }
    }
    
    // 清理
     // 安全释放内存
    if (pcm_buffer) {
        heap_caps_free(pcm_buffer);
        pcm_buffer = nullptr;
    }
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
        mp3_input_buffer = nullptr;
    }
    
    // 清理全局FFT数据指针
    if (final_pcm_data_fft) {
        heap_caps_free(final_pcm_data_fft);
        final_pcm_data_fft = nullptr;
    }
    
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");
    
    // 停止播放标志
    is_playing_ = false;
    
    // 只在频谱显示模式下才停止FFT显示
    if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->stopFft();
            ESP_LOGI(TAG, "Stopped FFT display from play thread (spectrum mode)");
        }
    } else {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop");
    }
    ResetSampleRate();
    // 添加：歌曲播放完毕后，设备进入聆听状态
    vTaskDelay(pdMS_TO_TICKS(300));
    auto& app = Application::GetInstance();
    app.ToggleChatState();
    ESP_LOGI(TAG, "Playback finished, switching to listening state");
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
            chunk.data = nullptr;  // 避免悬空指针
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    // 检查URL是否为空
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        // 打开GET连接
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }
        
        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);
        
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");
        
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }
        
        http->Close();
        
        if (read_error) {
            retry_count++;
            continue;
        }
        
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }
    
    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content, size: %d bytes", lyric_content.size());
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    std::string lrc_text;
    
    // 首先尝试解析JSON格式
    if (lyric_content.find("{\"lyric\":") == 0) {
        ESP_LOGI(TAG, "Detected JSON format lyrics, extracting lyric field");
        
        try {
            // 简单的JSON解析，提取lyric字段
            size_t lyric_start = lyric_content.find("\"lyric\":\"");
            if (lyric_start != std::string::npos) {
                lyric_start += 9; // 跳过 "\"lyric\":\""
                size_t lyric_end = lyric_content.find("\"", lyric_start);
                if (lyric_end != std::string::npos) {
                    lrc_text = lyric_content.substr(lyric_start, lyric_end - lyric_start);
                    
                    // 处理转义字符
                    size_t pos = 0;
                    while ((pos = lrc_text.find("\\n", pos)) != std::string::npos) {
                        lrc_text.replace(pos, 2, "\n");
                        pos += 1;
                    }
                    
                    ESP_LOGI(TAG, "Extracted LRC text, size: %d bytes", lrc_text.size());
                }
            }
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "JSON parsing failed: %s", e.what());
        }
    } else {
        // 如果不是JSON，直接使用原内容
        lrc_text = lyric_content;
    }
    
    if (lrc_text.empty()) {
        ESP_LOGE(TAG, "No LRC text extracted");
        return false;
    }
    
    // 按行分割歌词内容
    std::istringstream stream(lrc_text);
    std::string line;
    int parsed_lines = 0;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        ESP_LOGD(TAG, "Processing line: %s", line.c_str());
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos && close_bracket > 1) {
                std::string time_str = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = time_str.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = time_str.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是字母（元数据标签）
                    bool is_metadata = false;
                    for (char c : left_part) {
                        if (isalpha(c)) {
                            is_metadata = true;
                            break;
                        }
                    }
                    
                    // 如果是元数据标签，跳过这一行
                    if (is_metadata) {
                        ESP_LOGD(TAG, "Skipping metadata: [%s]%s", time_str.c_str(), content.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(left_part);
                        float seconds = std::stof(time_str.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 去除歌词文本前后的空白字符
                        size_t first_non_space = content.find_first_not_of(" \t\r\n");
                        size_t last_non_space = content.find_last_not_of(" \t\r\n");
                        
                        if (first_non_space != std::string::npos) {
                            content = content.substr(first_non_space, last_non_space - first_non_space + 1);
                        } else {
                            content.clear();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, content));
                        parsed_lines++;
                        
                        if (!content.empty()) {
                            // 限制日志输出长度
                            size_t log_len = std::min(content.length(), size_t(30));
                            std::string log_text = content.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%02d:%06.3f] %s", 
                                    minutes, seconds, log_text.c_str());
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time '%s': %s", time_str.c_str(), e.what());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Successfully parsed %d lyric lines from %d total lines", 
             parsed_lines, lyrics_.size());
    
    // 输出前几行歌词用于调试
    for (size_t i = 0; i < std::min(lyrics_.size(), size_t(5)); i++) {
        int minutes = lyrics_[i].first / 60000;
        int seconds = (lyrics_[i].first % 60000) / 1000;
        int ms = lyrics_[i].first % 1000;
        ESP_LOGI(TAG, "Lyric %d: [%02d:%02d.%03d] %s", 
                 i, minutes, seconds, ms, lyrics_[i].second.c_str());
    }
    
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            
            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());
            
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
                    current_time_ms, 
                    lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 * 
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Display mode changed from %s to %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
            (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}




bool Esp32Music::PlayLocalFile(const std::string& file_path) {
    // 检查文件是否存在
    struct stat st;
    if (stat(file_path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "File does not exist: %s", file_path.c_str());
        return false;
    }
    
    if (st.st_size == 0) {
        ESP_LOGE(TAG, "File is empty: %s", file_path.c_str());
        return false;
    }

    // 停止之前的播放
    StopStreaming();

    // 等待所有线程完全停止
    WaitForThreadsToFinish();

    // 清空缓冲区
    ClearAudioBuffer();

    // 重置MP3解码器
    CleanupMp3Decoder();
    if (!InitializeMp3Decoder()) {
        ESP_LOGE(TAG, "Failed to reinitialize MP3 decoder");
        return false;
    }
    // 配置线程栈大小
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 12288;  // 恢复到 12KB 以避免 Stack Overflow
    cfg.prio = 3;
    cfg.thread_name = "file_reader";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // 将堆栈分配到 PSRAM 中
    esp_pthread_set_cfg(&cfg);

    current_song_name_.clear();
    current_artist_name_.clear();
    current_song_name_ = "";
    current_artist_name_ = "";
    // 启动文件读取线程
    is_downloading_ = true;
    is_playing_ = true;
    
    download_thread_ = std::thread([this, file_path]() {
        FileReadingTask(file_path);
    });

    // 配置播放线程栈大小
    cfg.stack_size = 12288;  // 恢复到 12KB 以避免 Stack Overflow
    cfg.thread_name = "audio_player";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // 将堆栈分配到 PSRAM 中
    esp_pthread_set_cfg(&cfg);

    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Local file playback started successfully");
    return true;
}
//优化文件读取任务
void Esp32Music::FileReadingTask(const std::string& file_path) {
    ESP_LOGI(TAG, "File reading thread started for: %s", file_path.c_str());
    
    FILE* file = nullptr;
    uint8_t* buffer = nullptr;
    
    file = fopen(file_path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s, error: %s", 
                 file_path.c_str(), strerror(errno));
        is_downloading_ = false;
        return;
    }
    
    const size_t chunk_size = 2048;
    buffer = (uint8_t*)heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate file read buffer");
        fclose(file);
        is_downloading_ = false;
        return;
    }
    
    size_t total_read = 0;
    
    while (is_downloading_ && is_playing_) {
        size_t bytes_read = fread(buffer, 1, chunk_size, file);
        if (bytes_read == 0) {
            if (feof(file)) {
                ESP_LOGI(TAG, "File reading completed, total: %d bytes", total_read);
                break;
            } else if (ferror(file)) {
                ESP_LOGE(TAG, "File read error");
                break;
            }
        }
        
        // 创建音频数据块
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
        
        // 添加到缓冲区
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { 
                return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; 
            });
            
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_read += bytes_read;
                
                buffer_cv_.notify_one();
                
                if (total_read % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Read %d bytes from file", total_read);
                }
            } else {
                // 如果停止下载，释放刚分配的内存
                heap_caps_free(chunk_data);
                break;
            }
        }
    }
    
    // 安全清理
    if (buffer) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    if (file) {
        fclose(file);
        file = nullptr;
    }
    
    is_downloading_ = false;
    
    ESP_LOGI(TAG, "File reading finished, total bytes read: %d", total_read);
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
}

void Esp32Music::WaitForThreadsToFinish() {
    // 等待下载线程结束
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish...");
        
        is_downloading_ = false;
        
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // 使用更短的超时时间
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    // 等待播放线程结束
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish...");
        
        is_playing_ = false;
        
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
       
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    // 等待封面线程结束
    if (cover_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for cover thread to finish");
        is_cover_running_ = false;
        cover_thread_.join();
        ESP_LOGI(TAG, "Cover thread finished");
    }
}
// 添加安全检查方法
void Esp32Music::SafeMemoryCleanup() {
    if (!memory_cleaned_.exchange(true)) {
        // 只执行一次清理
        ClearAudioBuffer();
        CleanupMp3Decoder();
        // 清理封面数据
        if (final_pcm_data_fft) {
            heap_caps_free(final_pcm_data_fft);
            final_pcm_data_fft = nullptr;
        }
    }
}

static cJSON* select_song_by_artist(cJSON* response_json, const std::string& artist_name) {
    if (!response_json || !cJSON_IsArray(response_json)) {
        return nullptr;
    }
    
    cJSON* item;
    int array_size = cJSON_GetArraySize(response_json);
    if (array_size == 0) {
        return nullptr;
    }
    
    // 如果artist_name为空，直接返回第一个匹配歌名的
    if (artist_name.empty()) {
        // 可以添加歌名匹配逻辑，这里简单返回第一个
        return cJSON_GetArrayItem(response_json, 0);
    }
    
    // 第二优先级：包含匹配（考虑多个歌手的情况）
    cJSON_ArrayForEach(item, response_json) {
        cJSON* artist_json = cJSON_GetObjectItem(item, "singer");
        if (cJSON_IsString(artist_json)) {
            std::string artists = artist_json->valuestring;
            if (artists.find(artist_name) != std::string::npos) {
                return item;
            }
        }
    }
    
    
    // 默认返回第一个
    return cJSON_GetArrayItem(response_json, 0);
}

// 网络监控线程
void Esp32Music::NetworkMonitorThread() {
    ESP_LOGI(TAG, "Network monitor thread started");
    
    int consecutive_failures = 0;
    const int MAX_FAILURES = 5;
    
    while (is_playing_ && is_downloading_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 检查网络连接
        if (!CheckNetworkConnection()) {
            consecutive_failures++;
            ESP_LOGW(TAG, "Network check failed %d times", consecutive_failures);
            
            if (consecutive_failures >= MAX_FAILURES && !network_recovery_in_progress_) {
                ESP_LOGW(TAG, "Network seems disconnected, attempting recovery");
                network_recovery_in_progress_ = true;
                
                // 这里可以添加网络恢复逻辑，比如：
                // 1. 重新初始化WiFi
                // 2. 等待网络恢复
                // 3. 重新开始下载（从断点）
                
                network_recovery_in_progress_ = false;
                consecutive_failures = 0;
            }
        } else {
            consecutive_failures = 0;
        }
    }
    
    ESP_LOGI(TAG, "Network monitor thread finished");
}

bool Esp32Music::CheckNetworkConnection() {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }
    
    // 简单的ping测试（可以根据需要实现）
    // 这里简化处理，返回网络接口是否活跃
    return false;
}
