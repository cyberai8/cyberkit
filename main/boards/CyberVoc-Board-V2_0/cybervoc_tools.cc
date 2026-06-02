#include "cybervoc_tools.h"
#include "CyberVoc.h"
#include "cyber_base_control.h"
#include "audio_analysis.h"
#include "mcp_server.h"
#include "board.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include "customer_ui/alarm_api.h"

#define TAG "CyberVocTools"

void CyberVocTools::Initialize(EspS3Cat* board)
{
    auto &mcp_server = McpServer::GetInstance();

    // Cyber base action control
    mcp_server.AddTool("self.cyber_base.set_action", "Cyber base action control. Available actions:\n"
                       "shark_head: 摇头动作\n"
                       "shark_head_decay: 缓慢摇头动作\n"
                       "look_around: 环顾四周动作\n"
                       "cat_nuzzle: 蹭头撒娇动作\n"
                       "calibrate: 校准底座\n"
                       "turn_right: 向右转动作\n"
                       "turn_left: 向左转动作\n"
                       "cat_go_home: 归位回正动作\n",
    PropertyList({
        Property("action", kPropertyTypeString),
    }), [board](const PropertyList & properties) -> ReturnValue {
        const std::string &action = properties["action"].value<std::string>();
        int action_value = -1;

        ESP_LOGI(TAG, "&&& Do Action: %s", action.c_str());
        if (action == "shark_head") {
            action_value = CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD;
        } else if (action == "shark_head_decay") {
            action_value = CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD_DECAY;
        } else if (action == "look_around")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_LOOK_AROUND;
        } else if (action == "cat_nuzzle")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_NUZZLE;
        }  else if (action == "turn_right")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_TO_RIGHT;
        } else if (action == "turn_left")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_TO_LEFT;
        } else if (action == "cat_go_home")
        {
            action_value = CYBER_BASE_CMD_SET_ACTION_CAT_GO_HOME;
        } else if (action == "calibrate")
        {
            cyber_base_control_set_calibrate();
            BaseControl* base_control = board->GetBaseControl();
            if (base_control != nullptr) {
                bool completed = base_control->WaitForCalibrationComplete(30000);
                if (!completed) {
                    ESP_LOGW(TAG, "Calibration wait timeout");
                    return false;
                }
            }
        } else
        {
            return false;
        }

        if (action_value != -1)
        {
            esp_err_t ret = cyber_base_control_set_action(action_value);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set action: %d", ret);
                return false;
            }
        }
        return true;
    });

    // Audio analysis mode control
    mcp_server.AddTool("self.cyber_base.set_audio_mode", "Set audio analysis mode. Available modes:\n"
                       "doa_follow: DOA 声音方向跟随模式\n"
                       "disabled: 禁用音频分析",
    PropertyList({
        Property("mode", kPropertyTypeString),
    }), [board](const PropertyList & properties) -> ReturnValue {
        const std::string &mode = properties["mode"].value<std::string>();
        AudioAnalysisMode analysis_mode = AudioAnalysisMode::DISABLED;

        if (mode == "doa_follow") {
            analysis_mode = AudioAnalysisMode::DOA_FOLLOW;
        } else if (mode == "disabled")
        {
            analysis_mode = AudioAnalysisMode::DISABLED;
        } else
        {
            ESP_LOGE(TAG, "Unknown audio analysis mode: %s", mode.c_str());
            return false;
        }
    
        board->SetAudioAnalysisMode(analysis_mode);
        ESP_LOGI(TAG, "Audio analysis mode set to: %s", mode.c_str());
        return true;
    });

    // Pomodoro timer control
    mcp_server.AddTool("self.pomodoro.start", "开启番茄钟定时器，设置倒计时时间（1-60分钟，默认5分钟）",
    PropertyList({
        Property("minutes", kPropertyTypeInteger, 5, 1, 60),
    }), [](const PropertyList& properties) -> ReturnValue {
        int minutes = properties["minutes"].value<int>();
        ESP_LOGI(TAG, "Starting pomodoro timer with %d minutes", minutes);
        alarm_start_pomodoro(minutes);
        return true;
    });

    // Pomodoro timer control (start/pause)
    mcp_server.AddTool("self.pomodoro.control", "控制番茄钟定时器的运行状态。参数：start-启动定时器，pause-暂停定时器",
    PropertyList({
        Property("action", kPropertyTypeString),
    }), [](const PropertyList& properties) -> ReturnValue {
        const std::string &action = properties["action"].value<std::string>();
        
        bool success = false;
        if (action == "start") {
            ESP_LOGI(TAG, "Starting pomodoro timer");
            success = alarm_resume_pomodoro();
        } else if (action == "pause") {
            ESP_LOGI(TAG, "Pausing pomodoro timer");
            success = alarm_pause_pomodoro();
        } else {
            ESP_LOGE(TAG, "Unknown pomodoro action: %s (expected 'start' or 'pause')", action.c_str());
            return false;
        }
        return success;
    });

    // Sleep timer control
    mcp_server.AddTool("self.sleep.start", "设置睡眠闹钟，从当前时间到指定结束时间（24小时制）。开始时间自动为当前时间。参数：end_hour-结束时间的小时（0-23，默认8），end_min-结束时间的分钟（0-59，默认0）",
    PropertyList({
        Property("end_hour", kPropertyTypeInteger, 8, 0, 23),
        Property("end_min", kPropertyTypeInteger, 0, 0, 59),
    }), [](const PropertyList& properties) -> ReturnValue {
        int end_hour = properties["end_hour"].value<int>();
        int end_min = properties["end_min"].value<int>();
        
        // Validate parameters
        if (end_hour < 0 || end_hour >= 24) {
            ESP_LOGE(TAG, "Invalid end_hour: %d (must be 0-23)", end_hour);
            return false;
        }
        if (end_min < 0 || end_min >= 60) {
            ESP_LOGE(TAG, "Invalid end_min: %d (must be 0-59)", end_min);
            return false;
        }
        
        ESP_LOGI(TAG, "Setting sleep timer: current time -> %02d:%02d", end_hour, end_min);
        if(!alarm_start_sleep(end_hour, end_min))
        {
            return false;
        }
        return true;
    });
    mcp_server.AddTool("self.sleep.delete_clock", "用户明确要求删除睡眠闹钟时使用该工具",
        PropertyList({Property("delete", kPropertyTypeBoolean, true)}),[](const PropertyList& properties) -> ReturnValue 
        {
            
            if(alarm_delete_sleep())
            {
                return true;
            }
            else
            {
                return false;
            }
        }
    );
}
