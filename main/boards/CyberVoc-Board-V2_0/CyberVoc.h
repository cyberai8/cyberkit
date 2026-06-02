#ifndef CYBERVOC_H
#define CYBERVOC_H

#include "dual_network_board.h"
#include "display/display.h"
#include "backlight.h"
#include "button.h"
#include "touch_cst816s.h"
#include <esp_lcd_touch.h>
#include "base_control.h"
#include "audio_analysis.h"
#include "touch_sensor.h"
#include <functional>
#include "bq27220/bq27220.h"//电量监控芯片

class EspS3Cat : public DualNetworkBoard {
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

    // Power off by releasing the latch (PG2).
    void PowerOff();
private:
    i2c_master_bus_handle_t i2c_bus_;
    Cst816sTouch* cst816s_touch_ = nullptr;
    Button boot_button_;
    Display* display_;
    PwmBacklight* backlight_;

    BaseControl* base_control_;
    AudioAnalysis* audio_analysis_;
    TouchSensor* touch_sensor_ = nullptr;
    bool shake_enabled_ = true;

    i2c_bus_handle_t i2c_bus = NULL; // bq27220 uses this
    TaskHandle_t imu_task_handle_ = nullptr;
    bool qmi8658_ready_ = false;
    uint32_t last_shake_trigger_time_ = 0;
    const uint32_t SHAKE_COOLDOWN_MS = 10000; // match esp-vocat cooldown

    bq27220_handle_t bq27220 = NULL;
    TaskHandle_t power_key_task_handle_ = nullptr;
    TaskHandle_t head_touch_task_handle_ = nullptr;

    void InitializeI2c();
    void InitializeSpi();
    void Initializest77916Display(uint8_t pcb_verison);
    void InitializeButtons();
    void InitializeCharge();
    void InitializeCst816sTouchPad();
    void InitializeTouchSensor(uint8_t pcb_verison);
    void InitializeHeadTouchGpio();
    void InitializePowerKey();
    void InitializePowerLatch();
    void InitializePowerKeyGpio();
    int ReadPowerKeyLevel() const;
    bool IsPowerKeyPressed() const;
    void ShowPowerOverlay(const char* message);
    void HidePowerOverlay();
    bool ShowBootAnimation();
    void ShowHappyTouchFeedback();
    void InitializeQmi8658();
    void handle_violent_shake_event();
    static void imu_event_task(void* arg);
    static void boot_power_overlay_task(void* arg);
    static void head_touch_gpio_task(void* arg);

    void Initializebq27220();
    //初始化TF卡
    void InitializeTFCard();

    // void create_control_ui();
};

#endif // CYBERVOC_H
