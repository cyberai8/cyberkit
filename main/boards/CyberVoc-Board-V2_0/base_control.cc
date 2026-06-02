#include "base_control.h"
#include "CyberVoc.h"
#include "cyber_base_control.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "config.h"
#include "application.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "BaseControl"

BaseControl::BaseControl(EspS3Cat* board) : board_(board)
{
    cyber_base_online_ = false;
    last_heartbeat_time_ = 0;
    heartbeat_check_timer_ = nullptr;
    calibrate_semaphore_ = xSemaphoreCreateBinary();
    if (calibrate_semaphore_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create calibrate semaphore");
    }
}

BaseControl::~BaseControl()
{
    if (heartbeat_check_timer_ != nullptr) {
        esp_timer_stop(heartbeat_check_timer_);
        esp_timer_delete(heartbeat_check_timer_);
    }
    if (calibrate_semaphore_ != nullptr) {
        vSemaphoreDelete(calibrate_semaphore_);
    }
}

void BaseControl::Initialize(uint8_t pcb_verison)
{
    (void)pcb_verison;
    const cyber_base_control_config_t base_config = {
        .uart_num = UART_NUM_1,
        .tx_pin = UART1_TX,
        .rx_pin = UART1_RX,
        .baud_rate = 115200,
        .rx_buffer_size = 1024,
        .cmd_cb = CmdCallback,
        .user_ctx = this,
    };
    
    esp_err_t ret = cyber_base_control_init(&base_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize cyber base control");
        return;
    }

    // Initialize heartbeat monitoring timer
    const esp_timer_create_args_t heartbeat_timer_args = {
        .callback = &HeartbeatCheckTimerCallback,
        .arg = this,
        .name = "heartbeat_check"
    };
    ret = esp_timer_create(&heartbeat_timer_args, &heartbeat_check_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create heartbeat check timer");
        return;
    }

    // Start periodic timer to check heartbeat every 500ms
    ret = esp_timer_start_periodic(heartbeat_check_timer_, 500 * 1000);  // 500ms in microseconds
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start heartbeat check timer");
        esp_timer_delete(heartbeat_check_timer_);
        heartbeat_check_timer_ = nullptr;
        return;
    }

    // Initialize to offline state
    cyber_base_online_ = false;
    last_heartbeat_time_ = 0;
}

void BaseControl::HandleCommand(uint8_t cmd, uint8_t *data, int data_len)
{
    Display* display = board_->GetDisplay();
    if (display == nullptr) {
        return;
    }

    emote::EmoteDisplay* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);

    // if (cmd != CYBER_BASE_CMD_RECV_HEARTBEAT) {
        // printf("Handle: cmd=%02X, ", cmd);
        // for (int i = 0; i < data_len; i++) {
        //     printf("%02X ", data[i]);
        // }
        // printf("\n");
    // }

    switch (cmd) {
    case CYBER_BASE_CMD_RECV_SLIDE_SWITCH: {
        if (data_len >= 2) {
            auto &app = Application::GetInstance();

            uint16_t event = (data[0] << 8) | data[1];
            switch (event) {
            case CYBER_BASE_CMD_RECV_SWITCH_SLIDE_DOWN:
                ESP_LOGI(TAG, "Slide switch down");
                app.ToggleChatState();
                break;
            case CYBER_BASE_CMD_RECV_SWITCH_SLIDE_UP:
                ESP_LOGI(TAG, "Slide switch up");
                app.ToggleChatState();
                break;
            case CYBER_BASE_CMD_RECV_CALIBRATE_START:
                ESP_LOGI(TAG, "Calibrate start");
                display->SetChatMessage("system", Lang::Strings::CALIBRATING_STEP1);
                break;
            case CYBER_BASE_CMD_RECV_CALIBRATE_STEP1:
                ESP_LOGI(TAG, "Calibrate step 1 Done");
                display->SetChatMessage("system", Lang::Strings::CALIBRATING_STEP2);
                break;
            case CYBER_BASE_CMD_RECV_CALIBRATE_STEP2:
                {ESP_LOGI(TAG, "Calibrate step 2 Done");
                display->SetChatMessage("system", Lang::Strings::CALIBRATING_STEP3);
                if (calibrate_semaphore_ != nullptr) {
                    xSemaphoreGive(calibrate_semaphore_);
                }
                break;}
            case CYBER_BASE_CMD_RECV_SWITCH_FISH_ATTACHED:
                {ESP_LOGI(TAG, "Fish attached");
                emote_display->InsertAnimDialog("eat", 3500);
                break;}
            case CYBER_BASE_CMD_RECV_SWITCH_PAIR_DETECT:
                {ESP_LOGI(TAG, "Pair detect");
                auto& board = static_cast<EspS3Cat&>(Board::GetInstance());
                // if(board.GetAudioAnalysisMode() != AudioAnalysisMode::BEAT_DETECTION)
                // {
                //     display->SetEmotion("neutral");
                // };
                display->SetEmotion("neutral");
                cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_LOOK_AROUND);
                break;}
            default:
                ESP_LOGI(TAG, "Slide switch event: %d", event);
                break;
            }
        }
        break;
    }
    case CYBER_BASE_CMD_RECV_PERCEPTION: {
        ESP_LOGI(TAG, "Perception mode response, data_len=%d", data_len);
        break;
    }
    case CYBER_BASE_CMD_RECV_ACTION: {
        ESP_LOGI(TAG, "Action response, data_len=%d", data_len);
        uint16_t event = (data[0] << 8) | data[1];
        switch (event) {
        case CYBER_BASE_CMD_RECV_ACTION_DONE:
            ESP_LOGI(TAG, "Action done");
            break;
        default:
            ESP_LOGI(TAG, "Unknown action event: %d", event);
            break;
        }
        break;
    }
    case CYBER_BASE_CMD_RECV_HEARTBEAT: {
        uint16_t event = (data[0] << 8) | data[1];
        switch (event) {
        case CYBER_BASE_CMD_RECV_HEARTBEAT_ALIVE: {
            int64_t current_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
            bool was_offline = !cyber_base_online_;

            last_heartbeat_time_ = current_time;
            cyber_base_online_ = true;

            if (was_offline) {
                ESP_LOGI(TAG, "Cyber base connected (reinserted)");
                board_->SetAudioAnalysisMode(AudioAnalysisMode::DOA_FOLLOW);
                auto &app = Application::GetInstance();
                app.SetAecMode(kAecOff);
                emote_display->InsertAnimDialog("insert", 3000);
            }
            break;
        }
        }
        break;
    }
    default: {
        ESP_LOGI(TAG, "Unknown command: 0x%02X", cmd);
        break;
    }
    }
}

void BaseControl::HeartbeatCheckTimerCallback(void* arg)
{
    BaseControl* self = static_cast<BaseControl*>(arg);
    if (self == nullptr) {
        return;
    }

    // Skip check if we haven't received any heartbeat yet
    if (self->last_heartbeat_time_ == 0) {
        return;
    }

    int64_t current_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
    int64_t time_since_last_heartbeat = current_time - self->last_heartbeat_time_;

    // Check if heartbeat timeout (2 seconds = 4 missed heartbeats at 500ms interval)
    if (self->cyber_base_online_ && time_since_last_heartbeat > BaseControl::HEARTBEAT_TIMEOUT_MS) {
        self->cyber_base_online_ = false;
        ESP_LOGW(TAG, "Cyber base disconnected (timeout: %lld ms)", time_since_last_heartbeat);
        if(self->board_->GetAudioAnalysisMode() == AudioAnalysisMode::DOA_FOLLOW)
        {
            self->board_->SetAudioAnalysisMode(AudioAnalysisMode::DISABLED);

        }

        auto &app = Application::GetInstance();
        app.SetAecMode(kAecOnDeviceSide);
    }
}

bool BaseControl::WaitForCalibrationComplete(int timeout_ms)
{
    if (calibrate_semaphore_ == nullptr) {
        ESP_LOGE(TAG, "Calibrate semaphore not initialized");
        return false;
    }

    // Clear semaphore first in case it was already set
    xSemaphoreTake(calibrate_semaphore_, 0);

    // Wait for calibration to complete
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    BaseType_t result = xSemaphoreTake(calibrate_semaphore_, timeout_ticks);

    if (result == pdTRUE) {
        ESP_LOGI(TAG, "Calibration completed successfully");
        return true;
    } else {
        ESP_LOGW(TAG, "Calibration wait timeout after %d ms", timeout_ms);
        return false;
    }
}

void BaseControl::CmdCallback(uint8_t cmd, uint8_t *data, int data_len, void *user_ctx)
{
    BaseControl* self = static_cast<BaseControl*>(user_ctx);
    if (self != nullptr) {
        self->HandleCommand(cmd, data, data_len);
    }
}
