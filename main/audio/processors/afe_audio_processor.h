#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>
#include <mutex>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    /** Release the current AFE instance. Initialize() can rebuild it for a new input profile. */
    bool Deinitialize();
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    int frame_duration_ms_ = 0;
    srmodel_list_t* models_list_ = nullptr;
    bool is_speaking_ = false;
    std::mutex afe_data_mutex_;
    std::mutex state_mutex_;
    std::deque<int16_t> output_buffer_;
    std::atomic<int> active_fetch_count_{0};
    bool task_created_ = false;
    std::atomic<bool> aec_enabled_{false};
    bool afe_aec_available_ = false;
    bool afe_aec_enabled_ = false;
    std::atomic<bool> afe_control_dirty_{false};
    bool startup_stabilizing_ = false;
    int startup_frame_count_ = 0;

    void AudioProcessorTask();
    bool WaitForFetchIdle(TickType_t timeout_ticks);
    void ApplyAfeControlsLocked();
};

#endif // AFE_AUDIO_PROCESSOR_H
