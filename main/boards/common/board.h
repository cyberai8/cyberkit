#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"

#include "music.h"

#ifdef CONFIG_LANGUAGE_EN_US
#include "boards/CyberVoc-Board-V1_2-EN/touch_sensor.h"
#elifdef CONFIG_LANGUAGE_RU_RU
#include "boards/CyberVoc-Board-V1_2-RU/touch_sensor.h"
#elifdef CONFIG_LANGUAGE_ZH_CN
  #ifdef CONFIG_BOARD_TYPE_CYBERVOC_V2_0
  #include "boards/CyberVoc-Board-V2_0/touch_sensor.h"
  #else
  #include "boards/CyberVoc-Board-V1_2/touch_sensor.h"
  #endif
#endif

void *create_board();
class AudioCodec;
class Display;
class Board
{
private:
    Board(const Board &) = delete;            // 禁用拷贝构造函数
    Board &operator=(const Board &) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

    // 音乐播放器实例
    Music *music_;
    // 电容触摸传感器
    TouchSensor *touch_sensor_;

    bool shake_enabled_ = false;

public:
    static Board &GetInstance()
    {
        static Board *instance = static_cast<Board *>(create_board());
        return *instance;
    }

    virtual ~Board();
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight *GetBacklight() { return nullptr; }
    virtual Led *GetLed();
    virtual AudioCodec *GetAudioCodec() = 0;
    virtual bool GetTemperature(float &esp32temp);
    virtual Display *GetDisplay();
    virtual Camera *GetCamera();
    virtual NetworkInterface *GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual const char *GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveMode(bool enabled) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual void SetAfeDataProcessCallback(std::function<void(const int16_t *audio_data, size_t total_bytes)> callback) {}
    virtual void SetVadStateChangeCallback(std::function<void(bool speaking)> callback) {}
    virtual void SetAudioDataProcessedCallback(std::function<void(const int16_t *audio_data, size_t bytes_per_channel, size_t channels)> callback) {}

    virtual Music *GetMusic();
    virtual TouchSensor *GetTouchSensor();
    virtual bool IsShakeEnabled() const { return shake_enabled_; }
    virtual void SetShakeEnabled(bool enabled) { shake_enabled_ = enabled; }
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
    void *create_board()                \
    {                                   \
        return new BOARD_CLASS_NAME();  \
    }

#endif // BOARD_H
