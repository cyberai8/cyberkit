#include "afe_audio_processor.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01
#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

AfeAudioProcessor::~AfeAudioProcessor() {
    (void)Deinitialize();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms,
                                   srmodel_list_t* models_list) {
    if (!Deinitialize()) {
        ESP_LOGE(TAG, "Cannot initialize AFE while the previous fetch is still active");
        return;
    }

    codec_ = codec;
    frame_duration_ms_ = frame_duration_ms;
    models_list_ = models_list;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    const int reference_channels = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - reference_channels; ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < reference_channels; ++i) {
        input_format.push_back('R');
    }

    srmodel_list_t* models = models_list_;
    if (models == nullptr) {
        models = esp_srmodel_init("model");
    }
    char* ns_model_name = models == nullptr ? nullptr :
        esp_srmodel_filter(models, ESP_NSNET_PREFIX, nullptr);
    char* vad_model_name = models == nullptr ? nullptr :
        esp_srmodel_filter(models, ESP_VADN_PREFIX, nullptr);

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), nullptr,
                                                AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AFE config for %s", input_format.c_str());
        return;
    }
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }
    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->vad_init = true;

#if CONFIG_USE_DEVICE_AEC
    afe_aec_available_ = codec_->input_reference();
#else
    afe_aec_available_ = false;
#endif
    // MR creates AEC resources once so runtime toggles are safe. MM/DOA never
    // creates AEC because both channels are physical microphones.
    afe_config->aec_init = afe_aec_available_;

    const esp_afe_sr_iface_t* new_iface = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t* new_data = new_iface == nullptr ? nullptr :
        new_iface->create_from_config(afe_config);
    if (new_iface == nullptr || new_data == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE instance for input format %s", input_format.c_str());
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        afe_aec_available_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
        afe_iface_ = new_iface;
        afe_data_ = new_data;
        const bool want_aec = aec_enabled_.load(std::memory_order_acquire);
        afe_aec_enabled_ = afe_aec_available_ && want_aec;
        if (afe_aec_available_ && !want_aec) {
            afe_iface_->disable_aec(afe_data_);
        }
        afe_control_dirty_.store(false, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        output_buffer_.clear();
        is_speaking_ = false;
        startup_stabilizing_ = false;
        startup_frame_count_ = 0;
    }

    if (!task_created_) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        // Prefer PSRAM for the fetch task stack; internal SRAM is scarce after WiFi.
        const BaseType_t task_result = xTaskCreateWithCaps([](void* arg) {
            auto* self = static_cast<AfeAudioProcessor*>(arg);
            self->AudioProcessorTask();
            vTaskDeleteWithCaps(nullptr);
        }, "audio_communication", 4096, this, 3, nullptr,
           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        const BaseType_t task_result = xTaskCreate([](void* arg) {
            auto* self = static_cast<AfeAudioProcessor*>(arg);
            self->AudioProcessorTask();
            vTaskDelete(nullptr);
        }, "audio_communication", 4096, this, 3, nullptr);
#endif
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio communication task");
            Deinitialize();
            return;
        }
        task_created_ = true;
    }

    ESP_LOGI(TAG, "AFE capture profile %s: %d channels, AEC available=%d enabled=%d",
             input_format.c_str(), codec_->input_channels(),
             afe_aec_available_ ? 1 : 0, afe_aec_enabled_ ? 1 : 0);
}

bool AfeAudioProcessor::Deinitialize() {
    Stop();
    if (!WaitForFetchIdle(pdMS_TO_TICKS(500))) {
        ESP_LOGE(TAG, "AFE fetch did not stop; keeping the instance alive");
        return false;
    }
    std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
    if (afe_data_ != nullptr && afe_iface_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    afe_data_ = nullptr;
    afe_iface_ = nullptr;
    afe_aec_available_ = false;
    afe_aec_enabled_ = false;
    afe_control_dirty_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        output_buffer_.clear();
        is_speaking_ = false;
        startup_stabilizing_ = false;
        startup_frame_count_ = 0;
    }
    return true;
}

size_t AfeAudioProcessor::GetFeedSize() {
    std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr || !IsRunning() || data.empty()) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    if (event_group_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        startup_stabilizing_ = true;
        startup_frame_count_ = 0;
    }
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    if (event_group_ == nullptr) {
        return;
    }
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    // fetch_with_delay() can remain blocked after input feeding is disabled.
    // Reset its ring buffer first, then feed one silent chunk to guarantee that
    // an already waiting fetch has data with which to finish.
    {
        std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
        if (afe_data_ != nullptr && afe_iface_ != nullptr) {
            afe_iface_->reset_buffer(afe_data_);
            if (active_fetch_count_.load(std::memory_order_acquire) > 0 && codec_ != nullptr) {
                const size_t feed_samples =
                    afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
                if (feed_samples > 0) {
                    std::vector<int16_t> silence(feed_samples, 0);
                    afe_iface_->feed(afe_data_, silence.data());
                }
            }
        }
    }
    const bool fetch_idle = WaitForFetchIdle(pdMS_TO_TICKS(300));
    if (!fetch_idle) {
        ESP_LOGW(TAG, "Timed out waiting for AFE fetch to stop; continuing without blocking main task");
    } else {
        // Discard the silent wake-up frame before a later Start().
        std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
        if (afe_data_ != nullptr && afe_iface_ != nullptr) {
            afe_iface_->reset_buffer(afe_data_);
        }
    }
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        output_buffer_.clear();
        is_speaking_ = false;
        startup_stabilizing_ = false;
        startup_frame_count_ = 0;
    }
}

bool AfeAudioProcessor::IsRunning() {
    return event_group_ != nullptr &&
           (xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) != 0;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    output_callback_ = std::move(callback);
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioProcessor::AudioProcessorTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        esp_afe_sr_data_t* afe_data = nullptr;
        const esp_afe_sr_iface_t* afe_iface = nullptr;
        {
            std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
            if (afe_data_ == nullptr || afe_iface_ == nullptr || !IsRunning()) {
                continue;
            }
            if (afe_control_dirty_.exchange(false, std::memory_order_acq_rel)) {
                ApplyAfeControlsLocked();
            }
            afe_data = afe_data_;
            afe_iface = afe_iface_;
            active_fetch_count_.fetch_add(1, std::memory_order_acq_rel);
        }

        auto* result = afe_iface->fetch_with_delay(afe_data, pdMS_TO_TICKS(200));
        std::vector<int16_t> chunk;
        vad_state_t vad_state = VAD_SILENCE;
        bool fetch_ok = false;
        if (result != nullptr && result->ret_value != ESP_FAIL) {
            const size_t sample_count = result->data_size / sizeof(int16_t);
            chunk.assign(result->data, result->data + sample_count);
            vad_state = result->vad_state;
            fetch_ok = true;
        }
        active_fetch_count_.fetch_sub(1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> afe_lock(afe_data_mutex_);
            if (afe_data_ != afe_data || !IsRunning()) {
                continue;
            }
        }
        if (!fetch_ok) {
            continue;
        }

        std::vector<std::function<void()>> pending_callbacks;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (vad_state_change_callback_) {
                auto vad_callback = vad_state_change_callback_;
                if (vad_state == VAD_SPEECH && !is_speaking_) {
                    is_speaking_ = true;
                    pending_callbacks.emplace_back([vad_callback]() { vad_callback(true); });
                } else if (vad_state == VAD_SILENCE && is_speaking_) {
                    is_speaking_ = false;
                    pending_callbacks.emplace_back([vad_callback]() { vad_callback(false); });
                }
            }

            bool skip_output = false;
            if (startup_stabilizing_) {
                constexpr int kStartupIgnoreFrames = 10;
                ++startup_frame_count_;
                if (startup_frame_count_ < kStartupIgnoreFrames) {
                    skip_output = true;
                } else {
                    startup_stabilizing_ = false;
                    ESP_LOGI(TAG, "Startup stabilization complete");
                }
            }

            if (!skip_output && output_callback_ && !chunk.empty() && frame_samples_ > 0) {
                auto output_callback = output_callback_;
                output_buffer_.insert(output_buffer_.end(), chunk.begin(), chunk.end());
                while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
                    std::vector<int16_t> frame;
                    frame.reserve(frame_samples_);
                    for (int i = 0; i < frame_samples_; ++i) {
                        frame.push_back(output_buffer_.front());
                        output_buffer_.pop_front();
                    }
                    pending_callbacks.emplace_back(
                        [output_callback, frame = std::move(frame)]() mutable {
                            output_callback(std::move(frame));
                        });
                }
            }
        }

        for (auto& callback : pending_callbacks) {
            callback();
        }
    }
}

bool AfeAudioProcessor::WaitForFetchIdle(TickType_t timeout_ticks) {
    const TickType_t start = xTaskGetTickCount();
    while (active_fetch_count_.load(std::memory_order_acquire) > 0) {
        if (timeout_ticks == 0 ||
            static_cast<TickType_t>(xTaskGetTickCount() - start) >= timeout_ticks) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void AfeAudioProcessor::ApplyAfeControlsLocked() {
#if CONFIG_USE_DEVICE_AEC
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return;
    }
    const bool want_aec = aec_enabled_.load(std::memory_order_acquire);
    if (!afe_aec_available_) {
        if (want_aec) {
            ESP_LOGW(TAG, "Device AEC unavailable for dual-mic DOA profile");
        }
        afe_aec_enabled_ = false;
        return;
    }
    if (afe_aec_enabled_ == want_aec) {
        return;
    }
    if (want_aec) {
        afe_iface_->enable_aec(afe_data_);
    } else {
        afe_iface_->disable_aec(afe_data_);
    }
    afe_aec_enabled_ = want_aec;
    ESP_LOGI(TAG, "Device AEC %s", want_aec ? "enabled" : "disabled");
#endif
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    aec_enabled_.store(enable, std::memory_order_release);
#if CONFIG_USE_DEVICE_AEC
    afe_control_dirty_.store(true, std::memory_order_release);
#else
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported by this build");
    }
#endif
}
