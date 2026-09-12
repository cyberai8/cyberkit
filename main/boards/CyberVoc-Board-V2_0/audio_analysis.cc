#include "audio_analysis.h"

#include "application.h"
#include "board.h"
#include "cyber_base_control.h"
#include "display/emote_display.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "AudioAnalysis"

namespace {

constexpr float kMicSpacingM = 0.045085f;
constexpr float kDoaFmaxHz = 4000.0f;
constexpr int kDoaTaskPriority = 1;
constexpr uint32_t kDoaTaskStackSize = 8192 * 2;
// BoxAudioCodec exposes the detected ES7210 slots in ascending hardware order
// (slot 0, slot 2). On the CyberVoc microphone PCB those are right, left.
// ESP APA 2-mic geometry requires packed input in logical left, right order.
constexpr size_t kDoaLeftInputChannel = 1;
constexpr size_t kDoaRightInputChannel = 0;

}  // namespace

AudioAnalysis::AudioAnalysis()
{
}

AudioAnalysis::~AudioAnalysis()
{
    // AudioAnalysis is owned by the board singleton and lives until shutdown.
    // The DOA task intentionally remains alive with the rest of the audio stack.
}

AudioAnalysis::DoaChunk* AudioAnalysis::AllocateDoaChunk()
{
    DoaChunk* chunk = static_cast<DoaChunk*>(
        heap_caps_malloc(sizeof(DoaChunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (chunk == nullptr) {
        chunk = static_cast<DoaChunk*>(heap_caps_malloc(sizeof(DoaChunk), MALLOC_CAP_8BIT));
    }
    return chunk;
}

void AudioAnalysis::ReleaseDoaChunk(DoaChunk* chunk)
{
    if (chunk != nullptr) {
        heap_caps_free(chunk);
    }
}

void AudioAnalysis::Initialize()
{
    esp_apa_doa_cfg_t cfg = ESP_APA_DOA_CFG_DEFAULT();
    ESP_APA_DOA_PRESET_RESPONSIVE(&cfg);
    cfg.sample_rate_hz = 16000.0f;
    cfg.window_samples = static_cast<int>(kDoaWindowFrames);
    cfg.chunk_samples = static_cast<int>(kDoaChunkFrames);
    cfg.num_mics = static_cast<int>(kMicCount);
    cfg.mic_side_m = kMicSpacingM;
    cfg.fmax_hz = kDoaFmaxHz;
    cfg.input_layout = ESP_APA_DOA_INPUT_LAYOUT_PACKED;
    cfg.azimuth_offset_deg = 0.0f;

    esp_err_t ret = esp_apa_doa_create(&cfg, &doa_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ESP APA DOA engine: %s", esp_err_to_name(ret));
        doa_handle_ = nullptr;
        return;
    }

    doa_free_queue_ = xQueueCreate(kDoaQueueDepth, sizeof(DoaChunk*));
    doa_filled_queue_ = xQueueCreate(kDoaQueueDepth, sizeof(DoaChunk*));
    doa_input_mutex_ = xSemaphoreCreateMutex();
    if (doa_free_queue_ == nullptr || doa_filled_queue_ == nullptr || doa_input_mutex_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create APA DOA queues or mutex");
        if (doa_input_mutex_ != nullptr) {
            vSemaphoreDelete(doa_input_mutex_);
            doa_input_mutex_ = nullptr;
        }
        if (doa_free_queue_ != nullptr) {
            vQueueDelete(doa_free_queue_);
            doa_free_queue_ = nullptr;
        }
        if (doa_filled_queue_ != nullptr) {
            vQueueDelete(doa_filled_queue_);
            doa_filled_queue_ = nullptr;
        }
        esp_apa_doa_destroy(doa_handle_);
        doa_handle_ = nullptr;
        return;
    }

    for (size_t i = 0; i < kDoaQueueDepth; ++i) {
        doa_pool_[i] = AllocateDoaChunk();
        if (doa_pool_[i] == nullptr || xQueueSend(doa_free_queue_, &doa_pool_[i], 0) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to allocate APA DOA queue buffer %u", static_cast<unsigned>(i));
            for (size_t j = 0; j <= i && j < kDoaQueueDepth; ++j) {
                ReleaseDoaChunk(doa_pool_[j]);
                doa_pool_[j] = nullptr;
            }
            vSemaphoreDelete(doa_input_mutex_);
            doa_input_mutex_ = nullptr;
            vQueueDelete(doa_free_queue_);
            vQueueDelete(doa_filled_queue_);
            doa_free_queue_ = nullptr;
            doa_filled_queue_ = nullptr;
            esp_apa_doa_destroy(doa_handle_);
            doa_handle_ = nullptr;
            return;
        }
    }

    doa_accumulator_ = AllocateDoaChunk();
    if (doa_accumulator_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate APA DOA input accumulator");
        for (DoaChunk*& chunk : doa_pool_) {
            ReleaseDoaChunk(chunk);
            chunk = nullptr;
        }
        vSemaphoreDelete(doa_input_mutex_);
        doa_input_mutex_ = nullptr;
        vQueueDelete(doa_free_queue_);
        vQueueDelete(doa_filled_queue_);
        doa_free_queue_ = nullptr;
        doa_filled_queue_ = nullptr;
        esp_apa_doa_destroy(doa_handle_);
        doa_handle_ = nullptr;
        return;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        DoaTaskEntry,
        "audio_doa_apa",
        kDoaTaskStackSize,
        this,
        kDoaTaskPriority,
        &doa_task_handle_,
        1);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create APA DOA task");
        ReleaseDoaChunk(doa_accumulator_);
        doa_accumulator_ = nullptr;
        for (DoaChunk*& chunk : doa_pool_) {
            ReleaseDoaChunk(chunk);
            chunk = nullptr;
        }
        vSemaphoreDelete(doa_input_mutex_);
        doa_input_mutex_ = nullptr;
        vQueueDelete(doa_free_queue_);
        vQueueDelete(doa_filled_queue_);
        doa_free_queue_ = nullptr;
        doa_filled_queue_ = nullptr;
        esp_apa_doa_destroy(doa_handle_);
        doa_handle_ = nullptr;
        return;
    }

    ESP_LOGI(TAG,
             "ESP APA DOA ready: %s, %u kHz, %u-mic packed, input map=%u(left)/%u(right), "
             "window=%u chunk=%u spacing=%.3f m",
             esp_apa_doa_profile_name(doa_handle_),
             static_cast<unsigned>(cfg.sample_rate_hz / 1000.0f),
             static_cast<unsigned>(kMicCount),
             static_cast<unsigned>(kDoaLeftInputChannel),
             static_cast<unsigned>(kDoaRightInputChannel),
             static_cast<unsigned>(kDoaWindowFrames),
             static_cast<unsigned>(kDoaChunkFrames),
             kMicSpacingM);
}

void AudioAnalysis::DoaTaskEntry(void* arg)
{
    static_cast<AudioAnalysis*>(arg)->DoaTask();
}

void AudioAnalysis::DoaTask()
{
    bool previous_speaking = false;
    size_t silent_chunks = 0;

    while (true) {
        DoaChunk* chunk = nullptr;
        if (xQueueReceive(doa_filled_queue_, &chunk, portMAX_DELAY) != pdTRUE || chunk == nullptr) {
            continue;
        }

        if (doa_reset_pending_) {
            esp_apa_doa_reset(doa_handle_);
            doa_reset_pending_ = false;
            previous_speaking = false;
            silent_chunks = 0;
        }

        Application& app = Application::GetInstance();
        const bool active = mode_ == AudioAnalysisMode::DOA_FOLLOW &&
                            app.GetDeviceState() == kDeviceStateListening;
        if (active) {
            const bool speaking = is_speaking_;
            if (speaking && !previous_speaking) {
                // Keep the engine's PCM history, but clear transient confidence/jump state.
                esp_apa_doa_reset(doa_handle_);
                silent_chunks = 0;
            }

            esp_apa_doa_result_t result = {};
            if (speaking) {
                silent_chunks = 0;
                if (esp_apa_doa_process(doa_handle_, chunk->samples, kDoaChunkSamples, &result) == ESP_OK) {
                    HandleDoaResult(result);
                }
            } else {
                // Silent chunks still update the APA sliding history, matching the reference pipeline.
                esp_apa_doa_process(doa_handle_, chunk->samples, kDoaChunkSamples, nullptr);
                if (++silent_chunks >= kDoaSilentResetChunks) {
                    esp_apa_doa_reset(doa_handle_);
                    silent_chunks = 0;
                }
            }
            previous_speaking = speaking;
        } else {
            previous_speaking = false;
            silent_chunks = 0;
        }

        // A chunk is owned by the worker while it is being processed. Return the
        // same buffer to the free pool after processing completes.
        if (xQueueSend(doa_free_queue_, &chunk, 0) != pdTRUE) {
            ReleaseDoaChunk(chunk);
        }
    }
}

void AudioAnalysis::FlushDoaInput()
{
    if (doa_input_mutex_ == nullptr) {
        return;
    }

    xSemaphoreTake(doa_input_mutex_, portMAX_DELAY);
    doa_accumulated_frames_ = 0;

    DoaChunk* chunk = nullptr;
    while (xQueueReceive(doa_filled_queue_, &chunk, 0) == pdTRUE) {
        if (chunk != nullptr) {
            if (xQueueSend(doa_free_queue_, &chunk, 0) != pdTRUE) {
                ReleaseDoaChunk(chunk);
            }
        }
    }
    doa_reset_pending_ = true;
    xSemaphoreGive(doa_input_mutex_);
}

void AudioAnalysis::QueueAudioData(const int16_t* audio_data, size_t bytes_per_channel, size_t channels)
{
    if (audio_data == nullptr || channels != kMicCount || bytes_per_channel == 0 ||
        (bytes_per_channel % sizeof(int16_t)) != 0 || doa_accumulator_ == nullptr ||
        doa_free_queue_ == nullptr || doa_filled_queue_ == nullptr || doa_input_mutex_ == nullptr) {
        return;
    }

    const size_t frames = bytes_per_channel / sizeof(int16_t);
    size_t source_frame = 0;

    xSemaphoreTake(doa_input_mutex_, portMAX_DELAY);
    while (source_frame < frames) {
        const size_t frames_to_copy = std::min(kDoaChunkFrames - doa_accumulated_frames_, frames - source_frame);
        for (size_t i = 0; i < frames_to_copy; ++i) {
            const int16_t* source = audio_data + (source_frame + i) * channels;
            int16_t* destination = doa_accumulator_->samples +
                                   (doa_accumulated_frames_ + i) * kMicCount;
            destination[0] = source[kDoaLeftInputChannel];
            destination[1] = source[kDoaRightInputChannel];
        }
        source_frame += frames_to_copy;
        doa_accumulated_frames_ += frames_to_copy;

        if (doa_accumulated_frames_ != kDoaChunkFrames) {
            continue;
        }

        DoaChunk* queued_chunk = nullptr;
        if (xQueueReceive(doa_free_queue_, &queued_chunk, 0) != pdTRUE || queued_chunk == nullptr) {
            ++doa_feed_drops_;
            if ((doa_feed_drops_ % 16) == 1) {
                ESP_LOGW(TAG, "APA DOA queue full, dropped %u chunks", static_cast<unsigned>(doa_feed_drops_));
            }
        } else {
            memcpy(queued_chunk->samples, doa_accumulator_->samples, sizeof(doa_accumulator_->samples));
            if (xQueueSend(doa_filled_queue_, &queued_chunk, 0) != pdTRUE) {
                if (xQueueSend(doa_free_queue_, &queued_chunk, 0) != pdTRUE) {
                    ReleaseDoaChunk(queued_chunk);
                }
                ++doa_feed_drops_;
            }
        }
        doa_accumulated_frames_ = 0;
    }
    xSemaphoreGive(doa_input_mutex_);
}

void AudioAnalysis::HandleDoaResult(const esp_apa_doa_result_t& result)
{
    if (!result.valid || !is_speaking_ || mode_ != AudioAnalysisMode::DOA_FOLLOW ||
        Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
        return;
    }

    angle_sum_ += result.azimuth;
    ++angle_count_;
    last_detected_angle_ = result.azimuth;
    has_valid_angle_ = true;

    const int64_t now = esp_timer_get_time();
    if (angle_count_ == 1 || std::fabs(result.azimuth - last_logged_angle_) >= 8.0f ||
        now - last_logged_angle_time_ >= 500000) {
        ESP_LOGI(TAG, "APA DOA valid: azimuth=%.1f deg, bin=%d, samples=%d",
                 result.azimuth, result.direction_bin, angle_count_);
        last_logged_angle_ = result.azimuth;
        last_logged_angle_time_ = now;
    }
}

void AudioAnalysis::SetAfeDataProcessCallback()
{
    auto& app = Application::GetInstance();
    app.GetAudioService().SetAfeDataProcessedCallback([this](const int16_t* audio_data, size_t total_bytes) {
        OnAfeDataProcessed(audio_data, total_bytes);
    });
}

void AudioAnalysis::SetVadStateChangeCallback()
{
    auto& app = Application::GetInstance();
    app.GetAudioService().SetVadStateChangeCallback([this](bool speaking) {
        OnVadStateChange(speaking);
    });
}

void AudioAnalysis::SetAudioDataProcessedCallback()
{
    auto& app = Application::GetInstance();
    app.GetAudioService().SetAudioDataProcessedCallback(
        [this](const int16_t* audio_data, size_t bytes_per_channel, size_t channels) {
            OnAudioDataProcessed(audio_data, bytes_per_channel, channels);
        });
}

void AudioAnalysis::OnAfeDataProcessed(const int16_t* audio_data, size_t total_bytes)
{
    (void)audio_data;
    (void)total_bytes;
}

void AudioAnalysis::OnVadStateChange(bool speaking)
{
    if (mode_ != AudioAnalysisMode::DOA_FOLLOW || doa_handle_ == nullptr) {
        is_speaking_ = false;
        return;
    }

    if (speaking) {
        const int64_t now = esp_timer_get_time();
        if (!is_speaking_) {
            is_speaking_ = true;
            has_valid_angle_ = false;
            angle_sum_ = 0.0f;
            angle_count_ = 0;
            speaking_start_time_ = now;
        }
        ESP_LOGD(TAG, "VAD active");
        return;
    }

    if (!is_speaking_) {
        return;
    }

    is_speaking_ = false;
    const int duration_ms = static_cast<int>((esp_timer_get_time() - speaking_start_time_) / 1000);
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateListening && !has_sent_angle_ && angle_count_ >= 1) {
        if (duration_ms > 300) {
            pending_angle_ = angle_sum_ / angle_count_;
            has_pending_angle_ = true;
            ESP_LOGI(TAG, "Finished speaking: duration=%d ms, count=%d, pending azimuth=%.1f",
                     duration_ms, angle_count_, pending_angle_);
        } else {
            ESP_LOGI(TAG, "Ignored short speech for DOA: duration=%d ms, count=%d",
                     duration_ms, angle_count_);
        }
    }
}

void AudioAnalysis::SetMode(AudioAnalysisMode mode)
{
    if (mode_ == mode) {
        return;
    }

    mode_ = mode;
    if (mode != AudioAnalysisMode::DOA_FOLLOW) {
        is_speaking_ = false;
        FlushDoaInput();
    } else {
        // Force the first dual-mic frame to start a fresh listening session,
        // including when the base is inserted while already listening.
        is_speaking_ = false;
        has_sent_angle_ = false;
        has_valid_angle_ = false;
        has_pending_angle_ = false;
        last_detected_angle_ = 0.0f;
        angle_sum_ = 0.0f;
        angle_count_ = 0;
        last_device_state_ = kDeviceStateUnknown;
        FlushDoaInput();
        doa_reset_pending_ = true;
    }

    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);
        if (emote_display != nullptr) {
            emote_display->StopAnimDialog();
        }
    }
    ESP_LOGI(TAG, "Audio analysis mode set to: %d", static_cast<int>(mode));
}

void AudioAnalysis::OnAudioDataProcessed(const int16_t* audio_data, size_t bytes_per_channel, size_t channels)
{
    auto& app = Application::GetInstance();
    const DeviceState current_state = app.GetDeviceState();

    if (current_state == kDeviceStateListening && last_device_state_ != kDeviceStateListening) {
        has_sent_angle_ = false;
        has_valid_angle_ = false;
        has_pending_angle_ = false;
        last_detected_angle_ = 0.0f;
        angle_sum_ = 0.0f;
        angle_count_ = 0;
        listening_start_time_ = esp_timer_get_time();
        FlushDoaInput();
        ESP_LOGI(TAG, "Entering Listening state, resetting APA DOA state");
    } else if (last_device_state_ == kDeviceStateListening && current_state != kDeviceStateListening) {
        if (!has_sent_angle_) {
            if (has_pending_angle_) {
                ESP_LOGI(TAG, "Leaving Listening, sending stored azimuth: %.1f", pending_angle_);
                cyber_base_control_set_angle(pending_angle_);
                has_sent_angle_ = true;
            } else if (is_speaking_ && angle_count_ >= 1) {
                const float avg_angle = angle_sum_ / angle_count_;
                ESP_LOGI(TAG, "Leaving Listening while speaking, sending azimuth: %.1f", avg_angle);
                cyber_base_control_set_angle(avg_angle);
                has_sent_angle_ = true;
            } else if (angle_count_ >= 1) {
                const float avg_angle = angle_sum_ / angle_count_;
                ESP_LOGI(TAG, "Leaving Listening, sending fallback azimuth: %.1f", avg_angle);
                cyber_base_control_set_angle(avg_angle);
                has_sent_angle_ = true;
            }
        }
        has_pending_angle_ = false;
        is_speaking_ = false;
        FlushDoaInput();
    }
    last_device_state_ = current_state;

    if (mode_ == AudioAnalysisMode::DOA_FOLLOW && current_state == kDeviceStateListening) {
        QueueAudioData(audio_data, bytes_per_channel, channels);
    }
}
