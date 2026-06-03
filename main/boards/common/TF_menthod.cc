#include "TF_menthod.h"
#include "application.h"

#include <fstream>

static const char *TAG = "TF_METHOD";

static constexpr int kMaxMp3ScanDepth = 4;
static constexpr size_t kMaxMp3ListResponseItems = 30;
static constexpr size_t kMp3ProbeBytes = 16 * 1024;

static bool IsSkippableDirName(const char *name)
{
    return strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0 ||
           strcmp(name, "System Volume Information") == 0 ||
           strcmp(name, ".Spotlight-V100") == 0 ||
           strcmp(name, ".Trashes") == 0;
}

static std::string JsonEscape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

static bool HasMp3SyncWord(const uint8_t *data, size_t size)
{
    if (data == nullptr || size < 2) {
        return false;
    }

    for (size_t i = 0; i + 1 < size; ++i) {
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) {
            return true;
        }
    }
    return false;
}

static bool LooksLikePlayableMp3(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "Cannot open MP3 candidate: %s", path);
        return false;
    }

    uint8_t *probe = static_cast<uint8_t *>(heap_caps_malloc(kMp3ProbeBytes, MALLOC_CAP_8BIT));
    if (probe == nullptr) {
        fclose(f);
        ESP_LOGW(TAG, "No memory to probe MP3 candidate, keep file: %s", path);
        return true;
    }

    size_t read_size = fread(probe, 1, kMp3ProbeBytes, f);
    fclose(f);

    bool valid = HasMp3SyncWord(probe, read_size);
    heap_caps_free(probe);

    if (!valid) {
        ESP_LOGW(TAG, "Skip invalid MP3 candidate without sync word: %s", path);
    }
    return valid;
}

static void AddMp3File(std::vector<Mp3FileInfo> &mp3_files, const char *full_path, const char *file_name)
{
    const char *ext = strrchr(file_name, '.');
    if (ext == nullptr || strcasecmp(ext, ".mp3") != 0) {
        return;
    }
    if (!LooksLikePlayableMp3(full_path)) {
        return;
    }

    Mp3FileInfo file_info;
    std::string original_filename = file_name;
    size_t dot_pos = original_filename.rfind('.');
    file_info.filename = dot_pos != std::string::npos ? original_filename.substr(0, dot_pos) : original_filename;
    file_info.filesize = 0;
    file_info.longname = full_path;

    struct stat st;
    if (stat(full_path, &st) == 0) {
        file_info.filesize = st.st_size;
    }

    mp3_files.push_back(file_info);
    ESP_LOGD(TAG, "Found MP3 file: '%s', Path: %s, Size: %.2f KB",
             file_info.filename.c_str(), file_info.longname.c_str(), file_info.filesize / 1024.0);
}

static void ScanMp3FilesRecursive(const std::string &dir_path, int depth, std::vector<Mp3FileInfo> &mp3_files)
{
    DIR *dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Failed to open directory: %s", dir_path.c_str());
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (IsSkippableDirName(entry->d_name)) {
            continue;
        }

        std::string full_path = dir_path + "/" + entry->d_name;
        bool is_dir = entry->d_type == DT_DIR;
        bool is_reg = entry->d_type == DT_REG;

        if (entry->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(full_path.c_str(), &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_reg = S_ISREG(st.st_mode);
            }
        }

        if (is_reg) {
            AddMp3File(mp3_files, full_path.c_str(), entry->d_name);
        } else if (is_dir && depth < kMaxMp3ScanDepth) {
            ScanMp3FilesRecursive(full_path, depth + 1, mp3_files);
        }
    }

    closedir(dir);
}

static bool CollectMp3Files(std::vector<Mp3FileInfo> &mp3_files)
{
    struct stat mount_stat;
    if (stat(MOUNT_POINT, &mount_stat) != 0 || !S_ISDIR(mount_stat.st_mode)) {
        ESP_LOGE(TAG, "Failed to open directory: %s", MOUNT_POINT);
        return false;
    }

    mp3_files.clear();
    ScanMp3FilesRecursive(MOUNT_POINT, 1, mp3_files);
    ESP_LOGI(TAG, "Found %d MP3 files, scan depth: %d", mp3_files.size(), kMaxMp3ScanDepth);
    return true;
}

esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, "%s", data);
    fclose(f);
    ESP_LOGI(TAG, "File written");
    return ESP_OK;
}

esp_err_t s_example_read_file(const char *path)
{
    ESP_LOGI(TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    char line[EXAMPLE_MAX_CHAR_SIZE];
    fgets(line, sizeof(line), f);
    fclose(f);

    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);
    return ESP_OK;
}

std::string Get_mp3_files_json()
{
    std::vector<Mp3FileInfo> mp3_files;
    if (!CollectMp3Files(mp3_files)) {
        return "{\"success\":false,\"message\":\"Failed to open SD card music directory\"}";
    }

    const size_t response_count = mp3_files.size() > kMaxMp3ListResponseItems
                                      ? kMaxMp3ListResponseItems
                                      : mp3_files.size();
    std::string json = "{\"success\":true,\"total\":" + std::to_string(mp3_files.size()) +
                       ",\"returned\":" + std::to_string(response_count) +
                       ",\"limited\":";
    json += mp3_files.size() > response_count ? "true" : "false";
    json += ",\"message\":\"List is limited. Use self.sdcard.Play_music_from_sdcard to play SD music directly.\",\"mp3_files\":[";

    for (size_t i = 0; i < response_count; i++) {
        if (i > 0) {
            json += ",";
        }

        json += "{\"name\":\"" + JsonEscape(mp3_files[i].filename) + "\",";
        json += "\"path\":\"" + JsonEscape(mp3_files[i].longname) + "\",";
        json += "\"size\":\"" + std::to_string((int)(mp3_files[i].filesize / 1024.0)) + "KB\"}";
    }

    json += "]}";
    return json;
}

static esp_err_t PlayMp3File(const Mp3FileInfo &file_info)
{
    auto &board = Board::GetInstance();
    auto music_player = board.GetMusic();
    if (music_player == nullptr) {
        ESP_LOGE(TAG, "Music player is null");
        return ESP_FAIL;
    }

    music_player->StopStreaming();
    vTaskDelay(pdMS_TO_TICKS(500));

    if (music_player->PlayLocalFile(file_info.longname)) {
        ESP_LOGI(TAG, "Start local playback: %s", file_info.longname.c_str());
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Local playback failed: %s", file_info.longname.c_str());
    return ESP_FAIL;
}

esp_err_t Get_random_mp3_file()
{
    std::vector<Mp3FileInfo> mp3_files;
    if (!CollectMp3Files(mp3_files)) {
        ESP_LOGE(TAG, "Failed to collect MP3 files");
        return ESP_FAIL;
    }

    if (mp3_files.empty()) {
        ESP_LOGE(TAG, "No MP3 files found");
        return ESP_FAIL;
    }

    int random_index = esp_random() % mp3_files.size();
    const auto &selected = mp3_files[random_index];
    ESP_LOGI(TAG, "Random selected: %s -> %s", selected.filename.c_str(), selected.longname.c_str());
    return PlayMp3File(selected);
}

bool TF_Music_play(std::string music_name)
{
    ESP_LOGI(TAG, "Search song: %s", music_name.c_str());

    std::vector<Mp3FileInfo> mp3_files;
    if (!CollectMp3Files(mp3_files)) {
        ESP_LOGE(TAG, "Failed to collect MP3 files");
        return false;
    }

    for (const auto &file_info : mp3_files) {
        if (file_info.filename == music_name ||
            strcasecmp(file_info.filename.c_str(), music_name.c_str()) == 0 ||
            file_info.filename.find(music_name) != std::string::npos) {
            ESP_LOGI(TAG, "Song matched: %s -> %s", music_name.c_str(), file_info.longname.c_str());
            return PlayMp3File(file_info) == ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Song not found: %s", music_name.c_str());
    return false;
}
