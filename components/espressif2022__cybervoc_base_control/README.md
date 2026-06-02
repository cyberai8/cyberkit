# Cyberear Base Control Component

**ESP-IDF 组件 - Cyberear 底座控制上位机**

## 项目定位

本组件是 **Cyberear 底座的控制上位机组件**，运行在主控设备（如 ESP32）上，通过 UART 与底座进行通信，实现对底座的角度、动作和校准等功能的控制。

### 架构说明

```
┌─────────────────┐         UART          ┌─────────────────┐
│   主控设备       │  ←────────────────→   │   Cyberear 底座   │
│ (上位机/本组件)  │                       │   (下位机固件)   │
└─────────────────┘                       └─────────────────┘
```

- **上位机（本组件）：** 运行在主控设备上，负责发送控制命令和接收底座状态
- **下位机（底座固件）：** 运行在底座设备上，接收命令并执行相应动作

> **相关项目：**  
> 📦 **底座固件项目：** [esp-cybervoc-base](https://gitee.com/esp-friends/esp-cybervoc-base/blob/master/README_CN.md)  
> 📦 **控制上位机组件：** cybervoc_base_control（当前仓库）

## 功能特性

### 控制功能（发送命令）

- **角度控制**：设置底座旋转角度（0~65535）
- **动作控制**：支持多种预设动作
  - 摇头动作 (Shark Head)
  - 渐变递减摇头 (Shark Head Decay)
  - 左顾右盼 (Look Around)
  - 跟随节拍摇头 (Beat Swing)
  - 猫蹭脸动作 (Cat Nuzzle)
- **校准功能**：支持底座磁吸校准

### 状态反馈（接收命令）

- **滑动开关事件**：支持多种磁吸滑动开关事件
  - 初始化状态、上下滑动、移除/放置、单击
  - 小鱼挂件附着/分离检测
  - 配对检测/取消
- **动作完成通知**：动作执行完成回调
- **心跳检测**：底座在线状态检测
- **感知模式**：CSI 感知模式状态反馈
- **校准状态**：校准过程各阶段状态反馈

### 技术特性

- 可配置的 UART 端口、TX/RX 引脚和波特率
- 自动帧解析和命令处理
- 支持命令响应回调机制
- 完整的错误处理和资源管理

## 快速开始

### 项目链接

- 📦 **底座固件项目（下位机）：** [esp-cybervoc-base](https://gitee.com/esp-friends/esp-cybervoc-base/blob/master/README_CN.md)
- 📦 **控制上位机组件（本仓库）：** cybervoc_base_control

## 使用指南

### 初始化

```c
#include "cyber_base_control.h"

// 命令响应回调函数
void cmd_callback(uint8_t cmd, uint8_t *data, int data_len, void *user_ctx)
{
    switch (cmd) {
        case CYBER_BASE_CMD_RECV_SLIDE_SWITCH:
            // 处理滑动开关事件
            if (data_len >= 2) {
                uint16_t event = (data[0] << 8) | data[1];
                ESP_LOGI("APP", "Slide switch event: 0x%04X", event);
            }
            break;
        case CYBER_BASE_CMD_RECV_ACTION:
            // 处理动作完成通知
            ESP_LOGI("APP", "Action completed");
            break;
        case CYBER_BASE_CMD_RECV_HEARTBEAT:
            // 处理心跳
            ESP_LOGI("APP", "Base heartbeat received");
            break;
        default:
            ESP_LOGI("APP", "Received command: 0x%02X", cmd);
            break;
    }
}

// 配置并初始化
cyber_base_control_config_t config = {
    .uart_num = UART_NUM_1,
    .tx_pin = GPIO_NUM_5,
    .rx_pin = GPIO_NUM_4,
    .baud_rate = 115200,
    .rx_buffer_size = 1024,  // 可选，默认 1024
    .cmd_cb = cmd_callback,   // 命令响应回调（可为 NULL）
    .user_ctx = NULL,         // 用户上下文
};

esp_err_t ret = cyber_base_control_init(&config);
if (ret != ESP_OK) {
    ESP_LOGE("APP", "Failed to initialize cyber base control");
    return;
}
```

### 角度控制

```c
// 设置角度到 180 度
cyber_base_control_set_angle(180);
```

### 动作控制

```c
// 执行摇头动作
cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD);

// 执行左顾右盼动作
cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_LOOK_AROUND);

// 执行跟随节拍摇头
cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_BEAT_SWING);

// 执行猫蹭脸动作
cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_CAT_NUZZLE);

// 执行渐变递减摇头
cyber_base_control_set_action(CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD_DECAY);
```

### 校准功能

```c
// 启动校准
cyber_base_control_set_calibrate();
```

### 资源清理

```c
// 清理资源
cyber_base_control_deinit();
```

## 可用动作类型

| 动作常量 | 值 | 说明 |
|---------|-----|------|
| `CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD` | 0x0000 | 摇头动作 |
| `CYBER_BASE_CMD_SET_ACTION_SHARK_HEAD_DECAY` | 0x0001 | 渐变递减摇头 |
| `CYBER_BASE_CMD_SET_ACTION_LOOK_AROUND` | 0x0002 | 左顾右盼 |
| `CYBER_BASE_CMD_SET_ACTION_BEAT_SWING` | 0x0003 | 跟随节拍摇头 |
| `CYBER_BASE_CMD_SET_ACTION_CAT_NUZZLE` | 0x0004 | 猫蹭脸动作 |

## 滑动开关事件类型

| 事件常量 | 值 | 说明 |
|---------|-----|------|
| `CYBER_BASE_CMD_RECV_SWITCH_INIT` | 0x0000 | 初始化状态 |
| `CYBER_BASE_CMD_RECV_SWITCH_SLIDE_DOWN` | 0x0001 | 向下滑动 |
| `CYBER_BASE_CMD_RECV_SWITCH_SLIDE_UP` | 0x0002 | 向上滑动 |
| `CYBER_BASE_CMD_RECV_SWITCH_REMOVE_FROM_UP` | 0x0003 | 从上方移除 |
| `CYBER_BASE_CMD_RECV_SWITCH_REMOVE_FROM_DOWN` | 0x0004 | 从下方移除 |
| `CYBER_BASE_CMD_RECV_SWITCH_PLACE_FROM_UP` | 0x0005 | 从上方放置 |
| `CYBER_BASE_CMD_RECV_SWITCH_PLACE_FROM_DOWN` | 0x0006 | 从下方放置 |
| `CYBER_BASE_CMD_RECV_SWITCH_SINGLE_CLICK` | 0x0007 | 单击事件 |
| `CYBER_BASE_CMD_RECV_SWITCH_FISH_ATTACHED` | 0x0008 | 小鱼挂件附着 |
| `CYBER_BASE_CMD_RECV_SWITCH_FISH_DETACH` | 0x0009 | 小鱼挂件分离 |
| `CYBER_BASE_CMD_RECV_SWITCH_PAIR_DETECT` | 0x000A | 配对检测 |
| `CYBER_BASE_CMD_RECV_SWITCH_PAIR_CANCEL` | 0x000B | 配对取消 |

## 命令协议

### 发送命令（上位机 → 底座）

| 命令码 | 说明 | 数据长度 |
|--------|------|---------|
| `CYBER_BASE_CMD_SET_ANGLE` (0x01) | 设置角度 | 2 字节 |
| `CYBER_BASE_CMD_SET_ACTION` (0x02) | 设置动作 | 2 字节 |
| `CYBER_BASE_CMD_SET_CALIBRATE` (0x03) | 校准 | 2 字节 |

### 接收命令（底座 → 上位机）

| 命令码 | 说明 | 数据长度 |
|--------|------|---------|
| `CYBER_BASE_CMD_RECV_HEARTBEAT` (0x00) | 心跳 | 2 字节 |
| `CYBER_BASE_CMD_RECV_ACTION` (0x02) | 动作完成 | 2 字节 |
| `CYBER_BASE_CMD_RECV_SLIDE_SWITCH` (0x03) | 滑动开关事件 | 2 字节 |
| `CYBER_BASE_CMD_RECV_PERCEPTION` (0x06) | 感知模式 | 可变 |

## 通信协议

组件使用自定义帧协议：

- **帧头：** 0xAA 0x55
- **长度：** 2 字节（大端序）
- **命令码：** 1 字节
- **数据：** 可变长度
- **校验和：** 1 字节（命令码 + 所有数据字节的和）

## API 参考

完整的 API 文档请参考 `include/cyber_base_control.h`。

