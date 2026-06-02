#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>

#include <vector>
#include <string>
#include <functional>
#include  <mutex>

#include "board.h"

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

    //音乐
    //获取播放设备
    //virtual void SetOutputSampleRate(int rate);
    virtual void SetOutputChannels(int ch);
    //这个方法只是反不返回数据,并没有禁用通道
    virtual void SetOutputEnable(bool enable);
    virtual void SetInputEnable(bool enable);
    virtual esp_codec_dev_handle_t GetOutputDevice() const { 
        return nullptr; 
    }
    virtual esp_codec_dev_handle_t GetInputDevice() const { 
        return nullptr; 
    }
    virtual i2s_chan_handle_t GetTxHandle() const { 
        return nullptr; 
    }
    virtual i2s_chan_handle_t GetRxHandle() const { 
        return nullptr; 
    }
    inline int original_output_sample_rate() const { return original_output_sample_rate_; }
    virtual bool SetOutputSampleRate(int sample_rate);

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 2;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;

    int original_output_sample_rate_ = 0;
    std::mutex i2s_mutex_;
};

#endif // _AUDIO_CODEC_H
