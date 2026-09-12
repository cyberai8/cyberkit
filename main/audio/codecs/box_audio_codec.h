#ifndef _BOX_AUDIO_CODEC_H
#define _BOX_AUDIO_CODEC_H

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <sdkconfig.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>


class BoxAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;

#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
    bool doa_capture_mode_ = false;
    bool tdm_slot_map_ready_ = false;
    uint8_t tdm_slot0_ = 0;
    uint8_t tdm_slot1_ = 1;
    uint8_t tdm_candidate_slot0_ = 0;
    uint8_t tdm_candidate_slot1_ = 1;
    uint8_t tdm_candidate_stable_reads_ = 0;
    uint32_t tdm_probe_log_counter_ = 0;
    std::array<uint64_t, 4> tdm_last_energy_ = {0, 0, 0, 0};
    std::vector<int16_t> tdm_read_buffer_;
#endif

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    bool OpenInputDeviceLocked();
    void CloseInputDeviceLocked();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~BoxAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

    // 新增方法

    virtual bool SetOutputSampleRate(int rate) override;
    virtual void SetOutputChannels(int ch) override;
    virtual void SetOutputEnable(bool enable) override;
    virtual void SetInputEnable(bool enable) override;
#if CONFIG_BOARD_TYPE_CYBERVOC_V2_0
    /** true: read all four ES7210 TDM slots and output the two physical microphones. */
    bool SetDoaCaptureMode(bool enable);
    bool doa_capture_mode() const { return doa_capture_mode_; }
#endif
    esp_codec_dev_handle_t GetOutputDevice() const { return output_dev_; }
    esp_codec_dev_handle_t GetInputDevice() const { return input_dev_; }
    i2s_chan_handle_t GetTxHandle() const { return tx_handle_; }
    i2s_chan_handle_t GetRxHandle() const { return rx_handle_; }
};

#endif // _BOX_AUDIO_CODEC_H
