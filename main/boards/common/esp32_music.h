#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <lvgl.h>
#include "music.h"

//#define ENABLE_COVER_DOWNLOAD
// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}
class Http;
// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    std::string current_artist_name_;
    bool song_name_displayed_;

        //封面图像
    //CoverImage cover_image_;
     std::string current_cover_url_;
    std::string music_artist_;
    std::mutex cover_mutex_;
    std::atomic<bool> is_cover_running_;
    std::thread cover_thread_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    
    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB最小播放缓冲（降低以减少brownout风险）
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;

    //时长
    int total_duration_ms_ = 0;           // 音乐总时长
    
    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);


    //本地播放音频
    bool PlayLocalFile(const std::string& file_path);
    void FileReadingTask(const std::string& file_path);
    void WaitForThreadsToFinish();//添加线程等待方法
     // 添加内存安全检查标志
    std::atomic<bool> memory_cleaned_{false};
    
    // 添加安全检查方法
    void SafeMemoryCleanup();
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    int16_t* final_pcm_data_fft = nullptr;

    //音乐续传、网络监控
    std::atomic<bool> network_recovery_in_progress_;
    std::thread network_monitor_thread_;
    
    // 网络监控线程函数
    void NetworkMonitorThread();
    
    // 检查网络连接状态
    bool CheckNetworkConnection();

public:
    Esp32Music();
    ~Esp32Music();
    void ResetSampleRate() override;  // 重置采样率到原始值
    lv_img_dsc_t preview_image;
    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
    bool DownloadWithTimeout(const std::string& song_name, const std::string& artist_name, int timeout_ms);
    bool DownloadInternal(const std::string& song_name, const std::string& artist_name);
  
    virtual std::string GetDownloadResult() override;
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return final_pcm_data_fft; }
    
    // 显示模式控制方法
    bool MusicPlaying(){return is_playing_;}
    void SetDisplayMode(DisplayMode mode);
    virtual int GetTotalDuration() const { return total_duration_ms_; }
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }

    virtual std::string Get_song_name() override {return current_song_name_;}
    virtual std::string Get_artist_name() override {return current_artist_name_;}
};

#endif // ESP32_MUSIC_H
