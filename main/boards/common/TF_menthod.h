#ifndef _TF_MENTHOD_H_
#define _TF_MENTHOD_H_
#include <stdio.h>
#include <string>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "boards/common/esp32_music.h"
#define MOUNT_POINT              "/sdcard"
#define EXAMPLE_MAX_CHAR_SIZE    256
#define ESP_FAIL -1
esp_err_t s_example_write_file(const char *path, char *data);
esp_err_t s_example_read_file(const char *path);
esp_err_t Get_random_mp3_file();
std::string Get_mp3_files_json(); 

// 定义MP3文件信息结构体
struct Mp3FileInfo {
    std::string filename;    // 文件名（不含扩展名）
    size_t filesize;        // 文件大小
    std::string longname;   // 完整文件路径
};

// 辅助函数：获取长文件名
std::string GetLongFileName(const char* dirPath, const char* shortName);
bool TF_Music_play(std::string music_name);
#endif