#ifndef AUDIO_ANALYSIS_H
#define AUDIO_ANALYSIS_H

#include "audio_doa_app.h"
#include "device_state.h"
#include <sys/socket.h>
#include <netinet/in.h>

enum class AudioAnalysisMode {
    DOA_FOLLOW,      // DOA 人声音跟随模式
    DISABLED         // 禁用
};

class AudioAnalysis {
public:
    AudioAnalysis();
    ~AudioAnalysis();

    void Initialize();

    // Callback setup methods
    void SetAfeDataProcessCallback();
    void SetVadStateChangeCallback();
    void SetAudioDataProcessedCallback();

    // Mode control
    void SetMode(AudioAnalysisMode mode);
    AudioAnalysisMode GetMode() const
    {
        return mode_;
    }

    // Audio DOA
    audio_doa_app_handle_t GetDoaApp()
    {
        return doa_app_handle_;
    }

private:
    // Mode
    AudioAnalysisMode mode_ = AudioAnalysisMode::DISABLED;

    // Audio DOA
    audio_doa_app_handle_t doa_app_handle_;
    static void DoaTrackerResultCallback(float angle, void *ctx);

    // Internal handlers
    void OnAfeDataProcessed(const int16_t* audio_data, size_t total_bytes);
    void OnVadStateChange(bool speaking);
    void OnAudioDataProcessed(const int16_t* audio_data, size_t bytes_per_channel, size_t channels);

    bool has_sent_angle_ = false;
    DeviceState last_device_state_ = kDeviceStateUnknown;
    float last_detected_angle_ = 0.0f;
    bool has_valid_angle_ = false;
    bool is_speaking_ = false;
    float angle_sum_ = 0.0f;
    int angle_count_ = 0;
    int64_t speaking_start_time_ = 0;
    int64_t listening_start_time_ = 0;
    float pending_angle_ = 0.0f;
    bool has_pending_angle_ = false;
};

#endif // AUDIO_ANALYSIS_H
