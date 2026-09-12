#include "afe_wake_word.h"
#include "audio_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <sstream>

#define DETECTION_RUNNING_EVENT 1
#define OPUS_PACKET_READY_EVENT 2
#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {
    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    (void)Deinitialize();

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    // models_ is normally owned by AudioService and shared with the processor.
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    if (!Deinitialize()) {
        ESP_LOGE(TAG, "Cannot initialize WakeNet while the previous fetch is still active");
        return false;
    }

    codec_ = codec;
    if (models_list != nullptr) {
        models_ = models_list;
    } else if (models_ == nullptr) {
        models_ = esp_srmodel_init("model");
    }
    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    wake_words_.clear();
    wakenet_model_ = nullptr;
    for (int i = 0; i < models_->num; ++i) {
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) == nullptr) {
            continue;
        }
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        wakenet_model_ = models_->model_name[i];
        const auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
        std::stringstream stream(words);
        std::string word;
        while (std::getline(stream, word, ';')) {
            wake_words_.push_back(word);
        }
        break;
    }
    if (wakenet_model_ == nullptr) {
        ESP_LOGE(TAG, "No WakeNet model found");
        return false;
    }

    const int reference_channels = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - reference_channels; ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < reference_channels; ++i) {
        input_format.push_back('R');
    }

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_,
                                                AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate WakeNet AFE config");
        return false;
    }
    // Keep WakeNet lean: AEC is created by the voice processor when listening.
    // Enabling AEC here burns internal SRAM and starves LCD SPI DMA.
    afe_config->aec_init = false;
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_3;
    afe_config->vad_min_noise_ms = 64;
    afe_config->vad_init = true;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    const esp_afe_sr_iface_t* new_iface = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t* new_data = new_iface == nullptr ? nullptr :
        new_iface->create_from_config(afe_config);
    if (new_iface == nullptr || new_data == nullptr) {
        ESP_LOGE(TAG, "Failed to create WakeNet AFE for input format %s", input_format.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(afe_data_mutex_);
        afe_iface_ = new_iface;
        afe_data_ = new_data;
    }

    if (!detection_task_created_) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        const BaseType_t task_result = xTaskCreateWithCaps([](void* arg) {
            auto* self = static_cast<AfeWakeWord*>(arg);
            self->AudioDetectionTask();
            vTaskDeleteWithCaps(nullptr);
        }, "audio_detection", 4096, this, 3, nullptr,
           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        const BaseType_t task_result = xTaskCreate([](void* arg) {
            auto* self = static_cast<AfeWakeWord*>(arg);
            self->AudioDetectionTask();
            vTaskDelete(nullptr);
        }, "audio_detection", 4096, this, 3, nullptr);
#endif
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio detection task");
            Deinitialize();
            return false;
        }
        detection_task_created_ = true;
    }

    ESP_LOGI(TAG, "WakeNet capture profile %s: feed=%u fetch=%u",
             input_format.c_str(),
             static_cast<unsigned>(new_iface->get_feed_chunksize(new_data)),
             static_cast<unsigned>(new_iface->get_fetch_chunksize(new_data)));
    return true;
}

void AfeWakeWord::OnWakeWordDetected(
    std::function<void(const std::string& wake_word)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    wake_word_detected_callback_ = std::move(callback);
}

void AfeWakeWord::OnVadStateChange(std::function<void(bool speaking)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    vad_state_change_callback_ = std::move(callback);
}

void AfeWakeWord::OnAfeDataProcessed(
    std::function<void(const int16_t* audio_data, size_t total_bytes)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    afe_data_callback_ = std::move(callback);
}

void AfeWakeWord::Start() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
    }
}

void AfeWakeWord::Stop() {
    if (event_group_ == nullptr) {
        return;
    }
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);

    {
        std::lock_guard<std::mutex> lock(afe_data_mutex_);
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
        ESP_LOGW(TAG, "Timed out waiting for WakeNet fetch to stop");
    } else {
        std::lock_guard<std::mutex> lock(afe_data_mutex_);
        if (afe_data_ != nullptr && afe_iface_ != nullptr) {
            afe_iface_->reset_buffer(afe_data_);
        }
    }
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    is_speaking_ = false;
}

bool AfeWakeWord::Deinitialize() {
    Stop();
    if (!WaitForFetchIdle(pdMS_TO_TICKS(500))) {
        ESP_LOGE(TAG, "WakeNet fetch did not stop; keeping the instance alive");
        return false;
    }
    std::lock_guard<std::mutex> lock(afe_data_mutex_);
    if (afe_data_ != nullptr && afe_iface_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    afe_data_ = nullptr;
    afe_iface_ = nullptr;
    return true;
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(afe_data_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr || data.empty() ||
        (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    std::lock_guard<std::mutex> lock(afe_data_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        esp_afe_sr_data_t* afe_data = nullptr;
        const esp_afe_sr_iface_t* afe_iface = nullptr;
        {
            std::lock_guard<std::mutex> lock(afe_data_mutex_);
            if (afe_data_ == nullptr || afe_iface_ == nullptr ||
                (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
                continue;
            }
            afe_data = afe_data_;
            afe_iface = afe_iface_;
            active_fetch_count_.fetch_add(1, std::memory_order_acq_rel);
        }

        auto* result = afe_iface->fetch_with_delay(afe_data, pdMS_TO_TICKS(200));
        std::vector<int16_t> pcm;
        wakenet_state_t wakeup_state = WAKENET_NO_DETECT;
        int model_index = 0;
        vad_state_t vad_state = VAD_SILENCE;
        bool fetch_ok = false;
        if (result != nullptr && result->ret_value != ESP_FAIL) {
            const size_t samples = result->data_size / sizeof(int16_t);
            pcm.assign(result->data, result->data + samples);
            wakeup_state = result->wakeup_state;
            model_index = result->wakenet_model_index;
            vad_state = result->vad_state;
            fetch_ok = true;
        }
        active_fetch_count_.fetch_sub(1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> lock(afe_data_mutex_);
            if (afe_data_ != afe_data ||
                (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
                continue;
            }
        }
        if (!fetch_ok) {
            continue;
        }

        std::function<void(const int16_t*, size_t)> data_callback;
        std::function<void(bool)> vad_callback;
        std::function<void(const std::string&)> wake_callback;
        bool vad_changed = false;
        bool vad_value = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            data_callback = afe_data_callback_;
            if (vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_changed = true;
                vad_value = true;
                vad_callback = vad_state_change_callback_;
            } else if (vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_changed = true;
                vad_value = false;
                vad_callback = vad_state_change_callback_;
            }
            wake_callback = wake_word_detected_callback_;
        }

        if (data_callback) {
            data_callback(pcm.data(), pcm.size() * sizeof(int16_t));
        }
        if (vad_changed && vad_callback) {
            vad_callback(vad_value);
        }
        StoreWakeWordData(pcm.data(), pcm.size());

        if (wakeup_state == WAKENET_DETECTED) {
            if (model_index > 0 && static_cast<size_t>(model_index) <= wake_words_.size()) {
                last_detected_wake_word_ = wake_words_[model_index - 1];
            } else if (!wake_words_.empty()) {
                last_detected_wake_word_ = wake_words_.front();
            }
            Stop();
            if (wake_callback) {
                wake_callback(last_detected_wake_word_);
            }
        }
    }
}

bool AfeWakeWord::WaitForFetchIdle(TickType_t timeout_ticks) {
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

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    if (data == nullptr || samples == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_pcm_.emplace_back(data, data + samples);
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word_opus_.clear();
    }
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ =
            static_cast<StackType_t*>(heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto* self = static_cast<AfeWakeWord*>(arg);
        const auto start_time = esp_timer_get_time();
        std::deque<std::vector<int16_t>> pcm_snapshot;
        {
            std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
            pcm_snapshot = std::move(self->wake_word_pcm_);
            self->wake_word_pcm_.clear();
        }

        auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1,
                                                            OPUS_FRAME_DURATION_MS);
        encoder->SetComplexity(0);
        int packets = 0;
        for (auto& pcm : pcm_snapshot) {
            encoder->Encode(std::move(pcm), [self](std::vector<uint8_t>&& opus) {
                std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
                self->wake_word_opus_.emplace_back(std::move(opus));
                xEventGroupSetBits(self->event_group_, OPUS_PACKET_READY_EVENT);
            });
            ++packets;
        }

        const auto end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets,
                 static_cast<long>((end_time - start_time) / 1000));
        {
            std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
            self->wake_word_opus_.emplace_back();
            xEventGroupSetBits(self->event_group_, OPUS_PACKET_READY_EVENT);
        }
        vTaskDelete(nullptr);
    }, "encode_wake_word", stack_size, this, 2,
       wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            if (!wake_word_opus_.empty()) {
                opus.swap(wake_word_opus_.front());
                wake_word_opus_.pop_front();
                return !opus.empty();
            }
        }
        xEventGroupWaitBits(event_group_, OPUS_PACKET_READY_EVENT,
                            pdTRUE, pdFALSE, portMAX_DELAY);
    }
}
