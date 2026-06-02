#include "TF_menthod.h"
#include "application.h"
#include <fstream>
static const char *TAG = "TF_METHOD";
// 写文件内容 path是路径 data是内容
esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");   // 以只写方式打开路径中文件
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing"); 
        return ESP_FAIL;
    }
    fprintf(f, data); // 写入内容
    fclose(f);  // 关闭文件
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

// 读文件内容 path是路径
esp_err_t s_example_read_file(const char *path)
{
    ESP_LOGI(TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");  // 以只读方式打开文件
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE];  // 定义一个字符串数组
    fgets(line, sizeof(line), f); // 获取文件中的内容到字符串数组
    fclose(f); // 关闭文件

    // strip newline
    char *pos = strchr(line, '\n'); // 查找字符串中的“\n”并返回其位置
    if (pos) {
        *pos = '\0'; // 把\n替换成\0
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line); // 把数组内容输出到终端

    return ESP_OK;
}

// 使用智能指针避免大型局部变量占用栈空间
std::string Get_mp3_files_json()
{
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory");
        return "{\"error\": \"Failed to open directory\"}";
    }

    // 使用智能指针管理动态内存，避免栈溢出
    auto mp3_files = std::make_shared<std::vector<Mp3FileInfo>>();

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 "." 和 ".." 目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_type == DT_REG) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext != NULL && strcasecmp(ext, ".mp3") == 0) {
                Mp3FileInfo file_info;
                
                // 保留原始文件名（包括大小写）
                std::string original_filename = entry->d_name;
                
                // 移除.mp3扩展名
                size_t dot_pos = original_filename.rfind('.');
                if (dot_pos != std::string::npos) {
                    file_info.filename = original_filename.substr(0, dot_pos);
                } else {
                    file_info.filename = original_filename;
                }
                
                // 获取文件大小
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", MOUNT_POINT, entry->d_name);
                struct stat st;
                if (stat(full_path, &st) == 0) {
                    file_info.filesize = st.st_size;
                }
                
                // 存储完整路径
                file_info.longname = full_path;
                
                mp3_files->push_back(file_info);
                ESP_LOGI(TAG, "Found MP3 file: '%s', Size: %.2f KB", 
                         file_info.filename.c_str(), file_info.filesize / 1024.0);
            }
        }
    }
    closedir(dir);

    // 构建JSON响应，确保正确处理特殊字符
    std::string json = "{\"success\": true, \"mp3_files\":[";
    for (size_t i = 0; i < mp3_files->size(); i++) {
        if (i > 0) {
            json += ",";
        }
        
        // 对文件名进行JSON转义
        std::string escaped_name;
        for (char c : (*mp3_files)[i].filename) {
            if (c == '"' || c == '\\') {
                escaped_name += '\\';
            }
            escaped_name += c;
        }
        
        std::string escaped_path;
        for (char c : (*mp3_files)[i].longname) {
            if (c == '"' || c == '\\') {
                escaped_path += '\\';
            }
            escaped_path += c;
        }
        
        json += "{\"name\":\"" + escaped_name + "\",";
        json += "\"path\":\"" + escaped_path + "\",";
        json += "\"size\":\"" + std::to_string((int)((*mp3_files)[i].filesize/1024.0)) + "KB\"}";
    }
    json += "]}";

    ESP_LOGI(TAG, "Found %d MP3 files", mp3_files->size());
    return json;
}

// 修改为随机选择一个MP3文件
esp_err_t Get_random_mp3_file()
{
    std::string json = Get_mp3_files_json();
    if (json.find("\"error\"") != std::string::npos) {
        ESP_LOGE(TAG, "获取MP3文件列表失败");
        return ESP_FAIL;
    }

    // 使用cJSON解析，更可靠
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "JSON解析失败");
        return ESP_FAIL;
    }

    cJSON* mp3_files = cJSON_GetObjectItem(root, "mp3_files");
    if (!mp3_files || !cJSON_IsArray(mp3_files)) {
        ESP_LOGE(TAG, "找不到mp3_files数组");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int file_count = cJSON_GetArraySize(mp3_files);
    if (file_count == 0) {
        ESP_LOGE(TAG, "没有找到MP3文件");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // 随机选择
    int random_index = esp_random() % file_count;
    cJSON* selected_file = cJSON_GetArrayItem(mp3_files, random_index);
    
    cJSON* path = cJSON_GetObjectItem(selected_file, "path");
    cJSON* name = cJSON_GetObjectItem(selected_file, "name");
    
    if (!path || !cJSON_IsString(path)) {
        ESP_LOGE(TAG, "找不到文件路径");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    std::string file_path = path->valuestring;
    std::string song_name = name ? name->valuestring : "未知歌曲";
    
    ESP_LOGI(TAG, "随机选择: %s -> %s", song_name.c_str(), file_path.c_str());

    // 停止当前播放并开始新歌曲
    auto& board = Board::GetInstance();
    auto music_player = board.GetMusic();
    if (music_player) {
        music_player->StopStreaming();
        vTaskDelay(pdMS_TO_TICKS(500));
        
        if (music_player->PlayLocalFile(file_path)) {
            ESP_LOGI(TAG, "随机播放开始: %s", song_name.c_str());
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    cJSON_Delete(root);
    return ESP_FAIL;
}

// 根据音乐名称播放特定音乐 - 修复版本
bool TF_Music_play(std::string music_name)
{
    ESP_LOGI(TAG, "搜索歌曲: %s", music_name.c_str());
    
    std::string json = Get_mp3_files_json();
    if (json.find("\"error\"") != std::string::npos) {
        ESP_LOGE(TAG, "获取MP3文件列表失败");
        return false;
    }

    // 使用cJSON解析而不是字符串搜索
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "JSON解析失败");
        return false;
    }

    cJSON* mp3_files = cJSON_GetObjectItem(root, "mp3_files");
    if (!mp3_files || !cJSON_IsArray(mp3_files)) {
        ESP_LOGE(TAG, "找不到mp3_files数组");
        cJSON_Delete(root);
        return false;
    }

    int file_count = cJSON_GetArraySize(mp3_files);
    if (file_count == 0) {
        ESP_LOGE(TAG, "没有找到MP3文件");
        cJSON_Delete(root);
        return false;
    }

    std::string file_path;
    bool found = false;

    // 遍历所有文件进行匹配
    for (int i = 0; i < file_count; i++) {
        cJSON* file_item = cJSON_GetArrayItem(mp3_files, i);
        if (!file_item) continue;

        cJSON* name = cJSON_GetObjectItem(file_item, "name");
        cJSON* path = cJSON_GetObjectItem(file_item, "path");
        
        if (!name || !cJSON_IsString(name) || !path || !cJSON_IsString(path)) {
            continue;
        }

        std::string found_name = name->valuestring;
        ESP_LOGI(TAG, "找到歌曲: %s", found_name.c_str());

        // 精确匹配或忽略大小写匹配
        if (found_name == music_name || 
            strcasecmp(found_name.c_str(), music_name.c_str()) == 0) {
            file_path = path->valuestring;
            found = true;
            ESP_LOGI(TAG, "匹配成功: %s -> %s", music_name.c_str(), file_path.c_str());
            break;
        }
    }

    cJSON_Delete(root);

    if (!found) {
        ESP_LOGE(TAG, "未找到歌曲: %s", music_name.c_str());
        return false;
    }

    // 停止当前播放并开始新歌曲
    auto& board = Board::GetInstance();
    auto music_player = board.GetMusic();
    if (music_player) {
        music_player->StopStreaming();
        vTaskDelay(pdMS_TO_TICKS(500)); // 给停止操作一些时间
        
        if (music_player->PlayLocalFile(file_path)) {
            ESP_LOGI(TAG, "开始播放: %s", file_path.c_str());
            return true;
        }
    }

    ESP_LOGE(TAG, "播放失败: %s", file_path.c_str());
    return false;
}