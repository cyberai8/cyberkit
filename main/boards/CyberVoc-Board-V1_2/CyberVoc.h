#ifndef CYBERVOC_H
#define CYBERVOC_H

#include "wifi_board.h"
#include "display/display.h"
#include "backlight.h"
#include "button.h"
#include "touch_cst816s.h"
#include <esp_lcd_touch.h>
#include "base_control.h"
#include "audio_analysis.h"
#include "touch_sensor.h"
#include <functional>

#include "bmi270_fun.h"
#include "bq27220/bq27220.h"//电量监控芯片

class EspS3Cat : public WifiBoard {
public:
    EspS3Cat();

    virtual AudioCodec* GetAudioCodec() override;
    virtual Display* GetDisplay() override;
    virtual Backlight* GetBacklight() override;
    esp_lcd_touch_handle_t GetTouchpad();

    virtual void SetAfeDataProcessCallback(std::function<void(const int16_t* audio_data, size_t total_bytes)> callback) override;
    virtual void SetVadStateChangeCallback(std::function<void(bool speaking)> callback) override;
    virtual void SetAudioDataProcessedCallback(std::function<void(const int16_t* audio_data, size_t bytes_per_channel, size_t channels)> callback) override;

    void SetAudioAnalysisMode(AudioAnalysisMode mode);
    AudioAnalysisMode GetAudioAnalysisMode()
    {
        return audio_analysis_->GetMode();
    }
    AudioAnalysis* GetAudioAnalysis() const { return audio_analysis_; }
    BaseControl* GetBaseControl()
    {
        return base_control_;
    }
    bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual TouchSensor* GetTouchSensor() override
    {
        return touch_sensor_;
    }
    //virtual bool IsShakeEnabled() const { return shake_enabled_; }
    virtual void SetShakeEnabled(bool enabled) { shake_enabled_ = enabled; }
private:
    i2c_master_bus_handle_t i2c_bus_;
    Cst816sTouch* cst816s_touch_ = nullptr;
    Button boot_button_;
    Display* display_;
    PwmBacklight* backlight_;

    BaseControl* base_control_;
    AudioAnalysis* audio_analysis_;
    TouchSensor* touch_sensor_;
    bool shake_enabled_ = true;


    i2c_bus_handle_t i2c_bus = NULL; //bmi270使用
    Bmi270_fun* bmi_270= nullptr;
    bool is_shaking = false;  // 添加状态变量
    uint32_t last_shake_trigger_time_ = 0;
    const uint32_t SHAKE_COOLDOWN_MS = 5000; // 5秒冷却时间

    bq27220_handle_t bq27220 = NULL;

    void InitializeI2c();
    void InitializeSpi();
    void Initializest77916Display(uint8_t pcb_verison);
    void InitializeButtons();
    void InitializeCharge();
    void InitializeCst816sTouchPad();
    void InitializeTouchSensor(uint8_t pcb_verison);
    void InitializePower();
    void InitializeBMI270();
    void handle_violent_shake_event();
    static void shake_detector_task(void* arg);

    void Initializebq27220();
    //初始化TF卡
    void InitializeTFCard();

    uint8_t DetectPcbVersion();

    // void create_control_ui();
};

#endif // CYBERVOC_H
