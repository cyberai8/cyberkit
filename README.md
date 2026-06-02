# EchoEar（CyberVoc）ESP32-S3 固件

本仓库是基于开源项目 [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 的工程框架进行裁剪与二次开发的固件工程，**仅保留并维护**我们自己的两款硬件板级实现：

- `main/boards/CyberVoc-Board-V1_2`
- `main/boards/CyberVoc-Board-V2_0`

如需完整的多开发板支持与更通用的工程能力，请参考上游项目。

---

## 功能概览

- **ESP32-S3** 语音对话固件工程（ESP-IDF）
- 圆形触摸屏 UI（LVGL + `esp_lvgl_adapter`）
- 音频采集/播放、语音唤醒（依赖 `esp-sr`）
-（部分硬件）4G 模组支持与网络切换（板级能力）
- OTA（工程内含压缩 OTA 相关组件与示例/文档）

> 具体功能与交互细节，以板级 README 与代码实现为准。

---

## 支持的硬件

本仓库仅保证以下两款硬件的可用性：

- **CyberVoc-Board-V1_2**：`main/boards/CyberVoc-Board-V1_2`
- **CyberVoc-Board-V2_0**：`main/boards/CyberVoc-Board-V2_0`

对应的板级配置在各自目录下的 `config.h` / `config.json` 中。

---

## 快速开始（编译 / 烧录）

### 1) 准备 ESP-IDF 环境

- **要求**：ESP-IDF `>= 5.4.0`（详见 `main/idf_component.yml`）
- 建议使用与你当前工程验证一致的版本（例如日志中为 `v5.5.2`）

确保已正确安装并导出环境变量（`idf.py` 可用）。

### 2) 选择目标芯片

```bash
idf.py set-target esp32s3
```

### 3) 按板级编译（推荐）

本仓库的板级构建参数由 `main/boards/<board>/config.json` 描述，使用 `scripts/release.py` 进行自动化构建：

```bash
python scripts/release.py CyberVoc-Board-V1_2
# 或
python scripts/release.py CyberVoc-Board-V2_0
```

脚本会读取 `config.json` 并追加对应的 `sdkconfig` 片段，然后完成构建与打包。

可用以下命令查看脚本识别到的板级与变体：

```bash
python scripts/release.py --list-boards
```

### 4) 直接使用 idf.py 编译

```bash
idf.py build
```

> 本仓库已包含 `sdkconfig`（并随板级脚本追加差异配置），克隆后可直接编译。

如需手动选择板型与 UI 风格，可进入配置菜单：

```bash
idf.py menuconfig
```

然后在 `Xiaozhi Assistant` 相关菜单中选择 **Board Type** 等选项。

### 5) 烧录与串口监视

```bash
idf.py flash monitor
```

---

## 目录结构（精简说明）

- `main/`：应用主工程
  - `boards/`：板级实现
    - `CyberVoc-Board-V1_2/`
    - `CyberVoc-Board-V2_0/`
    - `common/`：通用板级抽象/复用（WiFi/4G/双网等）
- `components/`：工程依赖组件（含第三方组件与自研组件）
- `docs/`：协议/使用/扩展文档（如 MCP、WebSocket/MQTT 等）
- `scripts/`：构建/资源转换/发布脚本
- `partitions/`：分区表相关

板级说明请优先阅读：

- `main/boards/CyberVoc-Board-V2_0/README.md`
- `docs/custom-board.md`（上游风格的板级扩展说明）

---

## 上游项目与致谢

本工程框架沿用并参考了上游开源项目：

- 上游：[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)

我们在此基础上进行了**裁剪**（移除大量上游板级适配）与**板级定制开发**（仅保留 `CyberVoc-Board-V1_2` 与 `CyberVoc-Board-V2_0`）。

同时感谢 ESP-IDF、LVGL 及本工程所使用的各组件/库作者与贡献者。

---

## 许可证（License）

本仓库根目录提供 `LICENSE`，当前为 **MIT License**。

> 注意：本仓库包含多个第三方依赖与组件，它们可能具有各自的许可证（例如 `components/` 与部分子目录中存在独立 `LICENSE` 文件）。在二次分发时请一并遵守对应条款。

---

## 贡献（Contributing）

欢迎提交 Issue / PR。为便于维护：

- 仅保证 `CyberVoc-Board-V1_2` 与 `CyberVoc-Board-V2_0` 的可用性与回归
- 变更请尽量保持板级差异收敛在各自 `main/boards/<board>/` 目录内

