#include "box_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <algorithm>
#include <cinttypes>
#include <limits>

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    // CyberVoc normally captures microphone + playback reference. DOA switches
    // this profile at runtime to two physical microphones.
    (void)input_reference;
    input_reference_ = true;
    input_channels_ = 2;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

    ESP_LOGI(TAG, "input_reference: %d, input_channels: %d", input_reference_, input_channels_);
    ESP_LOGI(TAG, "input_sample_rate: %d, output_sample_rate: %d", input_sample_rate_, output_sample_rate_);

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

bool BoxAudioCodec::OpenInputDeviceLocked() {
    if (input_enabled_) {
        return true;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = static_cast<uint32_t>(input_sample_rate_),
        .mclk_multiple = 0,
    };
#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
    if (doa_capture_mode_) {
        // esp_codec_dev otherwise compacts only slots 0/1. DOA must inspect all
        // ES7210 slots because the two populated microphones vary by PCB wiring.
        fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                           ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
    }
#endif

    esp_err_t ret = esp_codec_dev_open(input_dev_, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open input device: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_codec_dev_set_in_gain(input_dev_, input_gain_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set input gain: %s", esp_err_to_name(ret));
        esp_codec_dev_close(input_dev_);
        return false;
    }

    AudioCodec::EnableInput(true);
    ESP_LOGI(TAG, "Input opened: mask=0x%x, rate=%" PRIu32 ", profile=%s",
             fs.channel_mask, fs.sample_rate,
#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
             doa_capture_mode_ ? "DOA dual-mic" : "AEC mic+ref"
#else
             "mic+ref"
#endif
    );
    return true;
}

void BoxAudioCodec::CloseInputDeviceLocked() {
    if (!input_enabled_) {
        return;
    }
    const esp_err_t ret = esp_codec_dev_close(input_dev_);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to close input device: %s", esp_err_to_name(ret));
    }
    AudioCodec::EnableInput(false);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        if (!OpenInputDeviceLocked()) {
            ESP_LOGE(TAG, "EnableInput failed; leaving input disabled");
        }
    } else {
        CloseInputDeviceLocked();
    }
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!input_enabled_ || input_dev_ == nullptr || dest == nullptr || samples <= 0) {
        return 0;
    }

#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
    if (doa_capture_mode_) {
        // Do not lock from idle ADC noise. A valid DOA utterance is already
        // gated near -55 dBFS, for which average absolute PCM is well above 32.
        constexpr uint64_t kMinAverageAbsForLock = 32;
        constexpr uint8_t kStableReadsToLock = 3;

        if ((samples % 2) != 0) {
            ESP_LOGW(TAG, "DOA read requires packed stereo samples, got %d", samples);
            return 0;
        }
        const size_t frames = static_cast<size_t>(samples / 2);
        tdm_read_buffer_.resize(frames * 4);
        const esp_err_t ret = esp_codec_dev_read(input_dev_, tdm_read_buffer_.data(),
                                                  tdm_read_buffer_.size() * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "DOA TDM read failed: %s", esp_err_to_name(ret));
            return 0;
        }

        if (!tdm_slot_map_ready_) {
            std::array<uint64_t, 4> energy = {0, 0, 0, 0};
            for (size_t frame = 0; frame < frames; ++frame) {
                for (size_t slot = 0; slot < energy.size(); ++slot) {
                    const int32_t sample = tdm_read_buffer_[frame * 4 + slot];
                    energy[slot] += static_cast<uint64_t>(sample >= 0 ? sample : -sample);
                }
            }
            tdm_last_energy_ = energy;

            uint8_t strongest = 0;
            for (uint8_t slot = 1; slot < 4; ++slot) {
                if (energy[slot] > energy[strongest]) {
                    strongest = slot;
                }
            }
            uint8_t second = strongest == 0 ? 1 : 0;
            for (uint8_t slot = 0; slot < 4; ++slot) {
                if (slot != strongest && energy[slot] > energy[second]) {
                    second = slot;
                }
            }

            // Keep channel order deterministic. Swapping the pair according to
            // the louder side would mirror the reported angle between utterances.
            const uint8_t candidate0 = std::min(strongest, second);
            const uint8_t candidate1 = std::max(strongest, second);
            if (candidate0 == tdm_candidate_slot0_ && candidate1 == tdm_candidate_slot1_) {
                if (tdm_candidate_stable_reads_ < std::numeric_limits<uint8_t>::max()) {
                    ++tdm_candidate_stable_reads_;
                }
            } else {
                tdm_candidate_slot0_ = candidate0;
                tdm_candidate_slot1_ = candidate1;
                tdm_candidate_stable_reads_ = 1;
            }
            tdm_slot0_ = candidate0;
            tdm_slot1_ = candidate1;

            const uint64_t second_average = frames == 0 ? 0 : energy[second] / frames;
            if (second_average >= kMinAverageAbsForLock &&
                tdm_candidate_stable_reads_ >= kStableReadsToLock) {
                tdm_slot_map_ready_ = true;
                ESP_LOGI(TAG,
                         "TDM mic slots locked: %u/%u, avg_abs=%u/%u/%u/%u",
                         static_cast<unsigned>(tdm_slot0_), static_cast<unsigned>(tdm_slot1_),
                         static_cast<unsigned>(energy[0] / frames),
                         static_cast<unsigned>(energy[1] / frames),
                         static_cast<unsigned>(energy[2] / frames),
                         static_cast<unsigned>(energy[3] / frames));
            } else if ((++tdm_probe_log_counter_ % 20) == 1) {
                ESP_LOGI(TAG,
                         "TDM mic probe: candidate=%u/%u stable=%u avg_abs=%u/%u/%u/%u",
                         static_cast<unsigned>(candidate0), static_cast<unsigned>(candidate1),
                         static_cast<unsigned>(tdm_candidate_stable_reads_),
                         static_cast<unsigned>(frames == 0 ? 0 : energy[0] / frames),
                         static_cast<unsigned>(frames == 0 ? 0 : energy[1] / frames),
                         static_cast<unsigned>(frames == 0 ? 0 : energy[2] / frames),
                         static_cast<unsigned>(frames == 0 ? 0 : energy[3] / frames));
            }
        }

        for (size_t frame = 0; frame < frames; ++frame) {
            dest[frame * 2] = tdm_read_buffer_[frame * 4 + tdm_slot0_];
            dest[frame * 2 + 1] = tdm_read_buffer_[frame * 4 + tdm_slot1_];
        }
        return samples;
    }
#endif

    const esp_err_t ret = esp_codec_dev_read(input_dev_, dest,
                                              static_cast<size_t>(samples) * sizeof(int16_t));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Input read failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return samples;
}

#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
bool BoxAudioCodec::SetDoaCaptureMode(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (doa_capture_mode_ == enable && input_reference_ == !enable && input_channels_ == 2) {
        return true;
    }

    const bool previous_mode = doa_capture_mode_;
    const bool previous_reference = input_reference_;
    const bool was_enabled = input_enabled_;
    if (was_enabled) {
        CloseInputDeviceLocked();
    }

    doa_capture_mode_ = enable;
    input_reference_ = !enable;
    input_channels_ = 2;
    tdm_slot_map_ready_ = false;
    tdm_slot0_ = 0;
    tdm_slot1_ = 1;
    tdm_candidate_slot0_ = 0;
    tdm_candidate_slot1_ = 1;
    tdm_candidate_stable_reads_ = 0;
    tdm_probe_log_counter_ = 0;
    tdm_last_energy_.fill(0);

    ESP_LOGI(TAG, "Capture profile: %s (input_reference=%d, channels=%d)",
             enable ? "DOA dual-mic" : "AEC mic+ref",
             input_reference_ ? 1 : 0, input_channels_);

    if (was_enabled && !OpenInputDeviceLocked()) {
        ESP_LOGE(TAG, "Capture profile switch failed; restoring previous profile");
        doa_capture_mode_ = previous_mode;
        input_reference_ = previous_reference;
        if (!OpenInputDeviceLocked()) {
            ESP_LOGE(TAG, "Failed to restore previous input profile");
        }
        return false;
    }
    return true;
}
#endif

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_) {
        return samples;
    }
    
    // 添加详细的参数检查
    if (output_dev_ == nullptr) {
        ESP_LOGE(TAG, "Output device is null");
        return samples;
    }
    
    if (data == nullptr) {
        ESP_LOGE(TAG, "Data pointer is null");
        return samples;
    }
    
    if (samples <= 0) {
        ESP_LOGW(TAG, "Invalid samples count: %d", samples);
        return samples;
    }
    
    size_t data_size = samples * sizeof(int16_t);
    if (data_size == 0 || data_size > 1024 * 1024) { // 合理的最大限制
        ESP_LOGE(TAG, "Invalid data size: %zu", data_size);
        return samples;
    }
    
    // 使用更安全的错误处理
    esp_err_t ret = esp_codec_dev_write(output_dev_, (void*)data, data_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_write failed: %s (0x%x)", esp_err_to_name(ret), ret);
        // 不中止程序，只记录错误
    }
    
    return samples;
}

//新增
bool BoxAudioCodec::SetOutputSampleRate(int rate) {
    return AudioCodec::SetOutputSampleRate(rate);
}

void BoxAudioCodec::SetOutputChannels(int ch) {
    AudioCodec::SetOutputChannels(ch);
}

void BoxAudioCodec::SetOutputEnable(bool enable) {
    AudioCodec::SetOutputEnable(enable);
}

void BoxAudioCodec::SetInputEnable(bool enable) {
    AudioCodec::SetInputEnable(enable);
}
