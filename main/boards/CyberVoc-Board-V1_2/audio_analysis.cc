#include "audio_analysis.h"
#include "audio_doa_app.h"
#include "application.h"
#include "board.h"
#include "audio_codec.h"
#include "display/emote_display.h"
#include "device_state.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <cstring>
#include "cyber_base_control.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <wifi_station.h>
#include "ui_bridge.h"

#define TAG "AudioAnalysis"

AudioAnalysis::AudioAnalysis() : doa_app_handle_(nullptr)
{
}

AudioAnalysis::~AudioAnalysis()
{
    // Handles will be cleaned up by their respective components
}

void AudioAnalysis::Initialize()
{
    // AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    // int channel_count = codec != nullptr ? codec->input_channels() : 1;
    // ESP_LOGI(TAG, "Initializing audio analysis with %d channels", channel_count);
    int channel_count = 2;

    // Initialize audio DOA
    audio_doa_app_config_t doa_app_cfg = {
        .audio_doa_result_callback = DoaTrackerResultCallback,
        .audio_doa_result_callback_ctx = this,
    };
    esp_err_t ret = audio_doa_app_create(&doa_app_handle_, &doa_app_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create audio DOA app");
    } else {
        ESP_LOGI(TAG, "Audio DOA app created successfully");
    }
}

void AudioAnalysis::DoaTrackerResultCallback(float angle, void *ctx)
{
    AudioAnalysis* self = static_cast<AudioAnalysis*>(ctx);
    if (self == nullptr) {
        return;
    }

    // Only update angle if we are currently speaking
    if (self->is_speaking_) {
        self->angle_sum_ += angle;
        self->angle_count_++;
        // Keep track of last angle as backup, though we prefer average
        self->last_detected_angle_ = angle; 
        
        ESP_LOGD(TAG, "Estimated direction: %.2f (Accumulating: count=%d, current_avg=%.2f)", 
                 angle, self->angle_count_, self->angle_sum_ / self->angle_count_);
    } else {
        ESP_LOGD(TAG, "Estimated direction: %.2f (Ignored, not speaking)", angle);
    }
}

void AudioAnalysis::SetAfeDataProcessCallback()
{
    auto &app = Application::GetInstance();
    app.GetAudioService().SetAfeDataProcessedCallback([this](const int16_t* audio_data, size_t total_bytes) {
        OnAfeDataProcessed(audio_data, total_bytes);
    });
}

void AudioAnalysis::SetVadStateChangeCallback()
{
    auto &app = Application::GetInstance();
    app.GetAudioService().SetVadStateChangeCallback([this](bool speaking) {
        OnVadStateChange(speaking);
    });
}

void AudioAnalysis::SetAudioDataProcessedCallback()
{
    auto &app = Application::GetInstance();
    app.GetAudioService().SetAudioDataProcessedCallback([this](const int16_t* audio_data, size_t bytes_per_channel, size_t channels) {
        OnAudioDataProcessed(audio_data, bytes_per_channel, channels);
    });
}

void AudioAnalysis::OnAfeDataProcessed(const int16_t* audio_data, size_t total_bytes)
{
}

void AudioAnalysis::OnVadStateChange(bool speaking)
{
    if (speaking) {
        int64_t now = esp_timer_get_time();
        
        // Removed 1000ms deadzone check to allow quick replies and interruption tails
        // We rely on duration (>300ms) and sample count (>=1) filters to reject noise

        ESP_LOGD(TAG, "active");
        
        // If we were already speaking, don't reset counters
        if (!is_speaking_) {
            is_speaking_ = true;
            has_valid_angle_ = false; 
            angle_sum_ = 0.0f;
            angle_count_ = 0;
            speaking_start_time_ = now;
        }
        
        // Always enable DOA processing when speaking, but we filter the output later
        audio_doa_app_set_vad_detect(doa_app_handle_, true);
    } else {
        // If we were not speaking (e.g. filtered by debounce), ignore silence too
        if (!is_speaking_) {
            return;
        }

        ESP_LOGD(TAG, "silence");
        is_speaking_ = false;
        // Don't disable immediately, let it process the last chunk
        audio_doa_app_set_vad_detect(doa_app_handle_, false);

        int64_t duration_us = esp_timer_get_time() - speaking_start_time_;
        int duration_ms = duration_us / 1000;

        // Send the angle only when user finishes speaking
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateListening) {
            if (!has_sent_angle_) {
                // Relaxed filtering: >300ms duration AND at least 1 sample
                // We trust the 1000ms initial deadzone to filter system noise
                if (duration_ms > 300 && angle_count_ >= 1) {
                    float avg_angle = angle_sum_ / angle_count_;
                    pending_angle_ = avg_angle;
                    has_pending_angle_ = true;
                    ESP_LOGI(TAG, "User finished speaking (duration=%dms, count=%d), stored avg direction: %.2f (Waiting for state change)", 
                             duration_ms, angle_count_, avg_angle);
                } else {
                    ESP_LOGI(TAG, "User finished speaking but ignored (duration=%dms, count=%d) - too short or not enough samples", 
                             duration_ms, angle_count_);
                }
            }
        }
    }
}

void AudioAnalysis::SetMode(AudioAnalysisMode mode)
{
    mode_ = mode;

    // Stop any existing animation dialog when mode changes
    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        emote::EmoteDisplay* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);
        if (emote_display != nullptr) {
            emote_display->StopAnimDialog();
        }
    }
    ESP_LOGI(TAG, "Audio analysis mode set to: %d", static_cast<int>(mode));
}

void AudioAnalysis::OnAudioDataProcessed(const int16_t* audio_data, size_t bytes_per_channel, size_t channels)
{
    const char *current_page = ui_bridge_get_current_page();
    if (current_page == NULL || strcmp(current_page, "DUMMY") != 0) {
        return;
    }

    auto &app = Application::GetInstance();
    DeviceState current_state = app.GetDeviceState();

    if (current_state == kDeviceStateListening && last_device_state_ != kDeviceStateListening) {
        has_sent_angle_ = false;
        has_valid_angle_ = false;
        has_pending_angle_ = false; // Reset pending angle
        last_detected_angle_ = 0.0f;
        angle_sum_ = 0.0f;
        angle_count_ = 0;
        listening_start_time_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Entering Listening state, resetting DOA angle sent flag");
    } else if (last_device_state_ == kDeviceStateListening && current_state != kDeviceStateListening) {
        // Exiting Listening state (e.g. to Speaking or Connecting)
        // Check if we have a pending angle to send
        if (!has_sent_angle_) {
             if (has_pending_angle_) {
                 ESP_LOGI(TAG, "Exiting Listening state, sending stored direction: %.2f", pending_angle_);
                 cyber_base_control_set_angle(pending_angle_);
                 has_sent_angle_ = true;
             } else if (is_speaking_ && angle_count_ >= 1) {
                 // Fast case: State changed while user was still speaking (or VAD didn't close yet)
                 float avg_angle = angle_sum_ / angle_count_;
                 ESP_LOGI(TAG, "Exiting Listening state (while speaking), sending current avg direction: %.2f", avg_angle);
                 cyber_base_control_set_angle(avg_angle);
                 has_sent_angle_ = true;
             } else if (angle_count_ >= 1) {
                 // Backup case: VAD closed, but pending logic failed (maybe duration too short but still valid command)
                 // If we have ANY data and state is changing, it means the command was accepted!
                 float avg_angle = angle_sum_ / angle_count_;
                 ESP_LOGI(TAG, "Exiting Listening state (accepted short command), sending fallback direction: %.2f", avg_angle);
                 cyber_base_control_set_angle(avg_angle);
                 has_sent_angle_ = true;
             }
        }
        has_pending_angle_ = false;
        is_speaking_ = false; // Reset speaking state as we left listening
    }
    last_device_state_ = current_state;

    //Incoming: bytes=1024, channels=2
    // ESP_LOGI(TAG, "Incoming: bytes=%d, channels=%d", bytes_per_channel, channels);

    switch (mode_) {
    case AudioAnalysisMode::DOA_FOLLOW: {
        if (current_state == kDeviceStateListening) {
            // Feed to audio DOA
            if (doa_app_handle_ != nullptr) {
                audio_doa_app_data_write(doa_app_handle_, (uint8_t *)audio_data, bytes_per_channel * channels);
            }
        }
        break;
    }

    case AudioAnalysisMode::DISABLED:
    default:
        break;
    }
}
