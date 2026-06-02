#ifndef BASE_CONTROL_H
#define BASE_CONTROL_H

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdint>

class EspS3Cat;
class Display;

class BaseControl {
public:
    BaseControl(EspS3Cat* board);
    ~BaseControl();

    void Initialize(uint8_t pcb_verison);
    bool IsOnline() const
    {
        return cyber_base_online_;
    }
    void HandleCommand(uint8_t cmd, uint8_t *data, int data_len);

    // Wait for calibration to complete (until CYBER_BASE_CMD_RECV_CALIBRATE_STEP2)
    // Returns true if calibration completed, false if timeout
    bool WaitForCalibrationComplete(int timeout_ms = 30000);

private:
    EspS3Cat* board_;
    bool cyber_base_online_;
    int64_t last_heartbeat_time_;
    esp_timer_handle_t heartbeat_check_timer_;
    SemaphoreHandle_t calibrate_semaphore_;
    static constexpr int64_t HEARTBEAT_TIMEOUT_MS = 2000;  // 2 seconds timeout

    static void HeartbeatCheckTimerCallback(void* arg);
    static void CmdCallback(uint8_t cmd, uint8_t *data, int data_len, void *user_ctx);
};

#endif // BASE_CONTROL_H
