#ifndef MUSIC_H
#define MUSIC_H

#include <string>

class Music {
public:
 // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 1     // 显示歌词
    };
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    
    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual int16_t* GetAudioData() = 0;
    virtual bool MusicPlaying()=0;
    virtual bool PlayLocalFile(const std::string& file_path)=0;
    virtual DisplayMode GetDisplayMode() const = 0;
    virtual int GetTotalDuration() const =0;
    virtual std::string Get_song_name() = 0;
    virtual std::string Get_artist_name() = 0;
    virtual void ResetSampleRate() = 0;
};

#endif // MUSIC_H 