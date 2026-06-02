# Audio DOA Component

一个用于 ESP32 的实时声源方向估计（Direction of Arrival, DOA）组件，通过分析双麦克风阵列的音频信号来确定声源的角度位置。

## 目录

- [功能特性](#功能特性)
- [快速开始](#快速开始)
- [API 参考](#api-参考)
- [算法原理](#算法原理)
- [配置参数](#配置参数)
- [注意事项](#注意事项)
- [依赖项](#依赖项)
- [许可证](#许可证)

## 功能特性

- ✅ **实时 DOA 计算**：基于双麦克风阵列实时计算声源方向（0-180度）
- ✅ **角度平滑处理**：使用高斯加权移动平均滤波减少角度抖动
- ✅ **角度校准**：内置非线性校准算法，提高边缘角度测量精度
- ✅ **角度跟踪器**：可选的 DOA Tracker 模块，提供更稳定的角度输出
- ✅ **异步处理**：使用独立 FreeRTOS 任务处理音频数据，不阻塞主流程
- ✅ **回调机制**：支持角度结果回调通知，便于集成
- ✅ **VAD 集成**：支持语音活动检测（VAD）控制，节省计算资源
- ✅ **线程安全**：使用 FreeRTOS StreamBuffer 进行线程安全的数据传输

## 快速开始

### 1. 添加依赖

在 `idf_component.yml` 中添加：

```yaml
dependencies:
  espressif/esp-sr: ~2.2.0
```

### 2. 基本使用

```c
#include "audio_doa_app.h"

// 角度结果回调函数
void doa_result_callback(float avg_angle, void *ctx) {
    ESP_LOGI("DOA", "Detected angle: %.2f degrees", avg_angle);
}

// 监控回调函数（可选）
void doa_monitor_callback(float angle, void *ctx) {
    // 实时监控原始角度
}

void app_main(void) {
    audio_doa_app_handle_t doa_app;
    audio_doa_app_config_t config = {
        .audio_doa_result_callback = doa_result_callback,
        .audio_doa_result_callback_ctx = NULL,  // 可以传递自定义上下文
        .audio_doa_monitor_callback = doa_monitor_callback,
        .audio_doa_monitor_callback_ctx = NULL,  // 可以传递自定义上下文
    };
    
    // 创建 DOA 应用实例
    ESP_ERROR_CHECK(audio_doa_app_create(&doa_app, &config));
    
    // 启用 VAD 检测（开始处理音频数据）
    ESP_ERROR_CHECK(audio_doa_app_set_vad_detect(doa_app, true));
    
    // 在音频数据回调中写入数据
    // audio_doa_app_data_write(doa_app, audio_data, data_size);
}
```

**注意**：回调上下文（`*_callback_ctx`）会原样传递给对应的回调函数，可以用于传递应用特定的数据或状态。

## API 参考

### audio_doa_app API

#### 创建和销毁

```c
esp_err_t audio_doa_app_create(audio_doa_app_handle_t *app, audio_doa_app_config_t *config);
esp_err_t audio_doa_app_destroy(audio_doa_app_handle_t app);
```

#### 启动和停止

```c
esp_err_t audio_doa_app_start(audio_doa_app_handle_t app);
esp_err_t audio_doa_app_stop(audio_doa_app_handle_t app);
```

#### 数据写入和 VAD 控制

```c
esp_err_t audio_doa_app_data_write(audio_doa_app_handle_t app, uint8_t *data, int bytes_size);
esp_err_t audio_doa_app_set_vad_detect(audio_doa_app_handle_t app, bool vad_detect);
```

### 回调函数类型

```c
// 最终结果回调（经过 Tracker 处理）
// @param avg_angle  平均角度值（0-180度）
// @param ctx        用户自定义上下文指针（从配置中传入）
typedef void (*audio_doa_result_callback_t)(float avg_angle, void *ctx);

// 监控回调（原始 DOA 角度）
// @param angle  原始角度值（0-180度）
// @param ctx    用户自定义上下文指针（从配置中传入）
typedef void (*audio_doa_monitor_callback_t)(float angle, void *ctx);
```

### 配置结构

```c
typedef struct {
    float                         distance;                        // 麦克风间距（默认0.046）
    audio_doa_monitor_callback_t  audio_doa_monitor_callback;      // 监控回调（可选，可为 NULL）
    void*                         audio_doa_monitor_callback_ctx;  // 监控回调上下文（可为 NULL）
    audio_doa_result_callback_t   audio_doa_result_callback;       // 结果回调（必需，不可为 NULL）
    void*                         audio_doa_result_callback_ctx;    // 结果回调上下文（可为 NULL）
} audio_doa_app_config_t;
```

**回调上下文说明**：
- `audio_doa_monitor_callback_ctx` 和 `audio_doa_result_callback_ctx` 会原样传递给对应的回调函数
- 可以传递 `NULL` 或不传递任何上下文
- 也可以传递指向应用特定数据结构的指针，用于在回调中访问应用状态
- 上下文指针的生命周期必须覆盖整个 DOA 应用实例的使用期间

## 算法原理

### DOA 计算流程

```
音频数据输入
    ↓
[1] 数据提取：分离左右通道
    ↓
[2] DOA 处理：esp_doa_process() 计算原始角度
    ↓
[3] 高斯滤波：移动加权平均（窗口大小=7，σ=1.0）
    ↓
[4] 角度校准：非线性校准算法
    ↓
[5] 回调输出：audio_doa_monitor_callback
    ↓
[6] Tracker 处理：进一步平滑和稳定
    ↓
[7] 最终输出：audio_doa_result_callback
```

### 角度校准算法

校准算法针对边缘角度（接近 0° 或 180°）进行非线性校正：

```
correction_factor = 1.0 + (|angle - 90°| / 90°) × 0.25
corrected_angle = 90° + (angle - 90°) × correction_factor
```

该算法提高了边缘角度的测量精度，因为边缘角度通常存在更大的测量误差。

### DOA Tracker 算法

Tracker 模块实现以下处理步骤：

1. **初始检测**：检测前 3 个样本，判断是否为正面朝向模式
2. **鲁棒均值**：使用排序和截断均值（去除最高和最低 10%）
3. **智能 90° 处理**：
   - 正面模式：直接接受 90° 输出
   - 非正面模式：需要持续 1000ms 的 90° 才接受
4. **角度量化**：将角度量化为 20° 的步长
5. **变化检测**：
   - 大角度变化（>30°）：重置缓冲区
   - 过滤小角度变化（<15°）

## 配置参数

### 默认参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 采样率 | 16000 Hz | 音频采样率 |
| 通道数 | 2 | 双通道（左右麦克风） |
| 数据格式 | 16-bit PCM | 音频数据格式 |
| 缓冲区大小 | 2048 字节 | 单次处理的数据量 |
| DOA 窗口大小 | 7 | 高斯滤波窗口大小 |
| 高斯 Sigma | 1.0 | 高斯滤波标准差 |
| Tracker 缓冲区 | 6 样本 | Tracker 内部缓冲区 |
| 角度量化步长 | 20° | Tracker 角度量化步长 |
| 输出间隔 | 1000 ms | Tracker 结果输出间隔 |

### 音频数据格式要求

- **格式**：16 位 PCM
- **采样率**：16000 Hz
- **通道**：双通道交错
- **字节序**：小端序

转换为 `int16_t` 后：
```
audio_buffer[0] = CH1 (左)
audio_buffer[1] = CH2 (右)
audio_buffer[2] = CH1 (左)
audio_buffer[3] = CH2 (右)
...
```

## 注意事项

### 内存管理

- 组件使用 FreeRTOS StreamBuffer 进行数据传输
- StreamBuffer 大小为 `2048 字节 × 3 = 6144 字节`
- 每个通道需要额外的缓冲区（约 1024 字节）
- 确保系统有足够的可用内存（建议至少 10KB 空闲堆内存）

### 音频格式要求

⚠️ **重要**：输入音频必须满足以下要求：
- 必须是 16 位 PCM 格式
- 采样率必须为 16kHz
- 必须是双通道交错格式
- 数据必须是小端序

### 线程安全

- `audio_doa_app_data_write()` 可以在任何线程中调用（线程安全）
- 角度结果通过回调函数返回，回调在 DOA 处理任务中执行
- **回调函数应尽量简短，避免阻塞**，否则可能影响实时性

### VAD 控制

- 使用 `audio_doa_app` 时，需要先启用 VAD 才会处理数据
- 当 `vad_detect = false` 时，`audio_doa_app_data_write()` 会直接返回，不处理数据
- 建议与语音活动检测模块集成，以节省 CPU 资源

### 性能考虑

- DOA 计算需要一定的 CPU 资源
- 建议在专用的 CPU 核心上运行音频处理任务
- 如果处理速度跟不上，可以调整任务优先级（当前为 10）
- 处理延迟约为 10-20ms（取决于系统负载）

### 角度范围

- **输出角度范围**：0-180 度
- **0 度**：声源在左侧（左麦克风方向）
- **90 度**：声源在正前方
- **180 度**：声源在右侧（右麦克风方向）

### 初始化顺序

正确的初始化顺序：

1. 调用 `audio_doa_app_create()` 创建实例
2. 调用 `audio_doa_app_set_vad_detect()` 启用 VAD（可选，但建议）
3. 开始调用 `audio_doa_app_data_write()` 写入数据

### 错误处理

所有 API 函数返回 `esp_err_t`，建议使用 `ESP_ERROR_CHECK()` 进行错误检查：

```c
esp_err_t ret = audio_doa_app_create(&doa_app, &config);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create DOA app: %s", esp_err_to_name(ret));
    return;
}
```

## 依赖项

### 必需依赖

- **esp-sr** (~2.2.0)：提供 `esp_doa` 核心算法
- **FreeRTOS**：用于任务管理和 StreamBuffer
- **ESP-IDF**：基础框架和内存管理

### 版本要求

- ESP-IDF v5.0 或更高版本
- esp-sr v2.2.0 或兼容版本

## 许可证

Apache-2.0

---

## 版本历史

### v1.0.0

- ✅ 基本的 DOA 角度计算功能
- ✅ 高斯滤波和角度校准
- ✅ DOA Tracker 角度跟踪
- ✅ 应用层封装
- ✅ VAD 集成支持

---

**项目地址**：[https://github.com/Zhang-HongZe/audio_doa](https://github.com/Zhang-HongZe/audio_doa)
