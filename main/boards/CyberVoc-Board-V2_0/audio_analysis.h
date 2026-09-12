#ifndef AUDIO_ANALYSIS_H
#define AUDIO_ANALYSIS_H

#include "device_state.h"
#include "esp_apa_doa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

private:
    static constexpr size_t kMicCount = 2;
    static constexpr size_t kDoaWindowFrames = 512;
    static constexpr size_t kDoaChunkFrames = 256;
    static constexpr size_t kDoaChunkSamples = kMicCount * kDoaChunkFrames;
    static constexpr size_t kDoaQueueDepth = 8;
    static constexpr size_t kDoaSilentResetChunks = 94;  // about 1.5 s at 16 kHz

    struct DoaChunk {
        int16_t samples[kDoaChunkSamples];
    };

    // Mode
    volatile AudioAnalysisMode mode_ = AudioAnalysisMode::DISABLED;

    // ESP APA DOA pipeline
    esp_apa_doa_handle_t doa_handle_ = nullptr;
    QueueHandle_t doa_free_queue_ = nullptr;
    QueueHandle_t doa_filled_queue_ = nullptr;
    SemaphoreHandle_t doa_input_mutex_ = nullptr;
    DoaChunk* doa_pool_[kDoaQueueDepth] = {};
    DoaChunk* doa_accumulator_ = nullptr;
    size_t doa_accumulated_frames_ = 0;
    TaskHandle_t doa_task_handle_ = nullptr;
    volatile bool doa_reset_pending_ = false;
    uint32_t doa_feed_drops_ = 0;

    // Internal handlers
    static void DoaTaskEntry(void* arg);
    void DoaTask();
    void QueueAudioData(const int16_t* audio_data, size_t bytes_per_channel, size_t channels);
    void FlushDoaInput();
    void HandleDoaResult(const esp_apa_doa_result_t& result);
    DoaChunk* AllocateDoaChunk();
    void ReleaseDoaChunk(DoaChunk* chunk);

    void OnAfeDataProcessed(const int16_t* audio_data, size_t total_bytes);
    void OnVadStateChange(bool speaking);
    void OnAudioDataProcessed(const int16_t* audio_data, size_t bytes_per_channel, size_t channels);

    bool has_sent_angle_ = false;
    DeviceState last_device_state_ = kDeviceStateUnknown;
    float last_detected_angle_ = 0.0f;
    bool has_valid_angle_ = false;
    volatile bool is_speaking_ = false;
    float angle_sum_ = 0.0f;
    int angle_count_ = 0;
    int64_t speaking_start_time_ = 0;
    int64_t listening_start_time_ = 0;
    float pending_angle_ = 0.0f;
    bool has_pending_angle_ = false;
    float last_logged_angle_ = 0.0f;
    int64_t last_logged_angle_time_ = 0;
};

#endif // AUDIO_ANALYSIS_H
