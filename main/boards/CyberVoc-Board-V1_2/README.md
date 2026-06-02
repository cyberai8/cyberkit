# CyberVoc-Board-V1_2

本目录包含 **CyberVoc-Board-V1_2** 的板级实现与配置文件。

## 概览

- **板级目录**：`main/boards/CyberVoc-Board-V1_2/`
- **目标芯片**：ESP32-S3（见 `config.json`）
- **网络能力**：Wi-Fi（本板级继承 `WifiBoard`，不包含 4G/双网板级切换逻辑）
- **显示与触摸**：圆形触摸屏（QSPI LCD + CST816S）

## 编译（推荐脚本方式）

板级构建参数由本目录的 `config.json` 描述，使用根目录脚本编译：

```bash
python scripts/release.py CyberVoc-Board-V1_2
```

## 关键文件

- `config.h`：引脚映射与硬件参数（音频 I2S、I2C、LCD/TP、SD 等）
- `config.json`：release 脚本构建变体与追加的 `sdkconfig` 片段
- `CyberVoc.cc/.h`：板级初始化与外设驱动集成
- `ui_bridge.*`：LVGL 页面与触摸手势桥接（与 UI 交互相关）

## 参考

- 根目录 `README.md`：项目总体说明、编译/烧录流程、License 与上游声明

