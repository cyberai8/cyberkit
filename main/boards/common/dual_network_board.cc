#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>
#include <driver/gpio.h>

static const char *TAG = "DualNetworkBoard";

static constexpr int kInitialMl307PowerSettleMs = 2000;
static constexpr int kInitialMl307DetectTimeoutMs = 3000;
static constexpr int kInitialMl307RetryDelayMs = 1000;
static constexpr int kInitialMl307DetectAttempts = 2;

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin,
                                   int32_t default_net_type, gpio_num_t ml307_power_en_pin,
                                   uart_port_t ml307_uart_num) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin),
      ml307_power_en_pin_(ml307_power_en_pin),
      ml307_uart_num_(ml307_uart_num) {
    
    {
        Settings settings("network", false);
        ml307_available_ = settings.GetBool("ml307_available", false);
        auto_select_done_ = settings.GetBool("auto_done", settings.GetInt("type", -1) != -1);
    }

    ConfigureMl307PowerPin();

    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); // 默认使用ML307 (1)
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
    settings.SetBool("auto_done", true);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    current_board_.reset();
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        SetMl307Power(true);
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_, ml307_uart_num_);
        ml307_available_ = false;
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        SetMl307Power(false);
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::ConfigureMl307PowerPin() {
    if (ml307_power_en_pin_ == GPIO_NUM_NC) {
        return;
    }
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << ml307_power_en_pin_);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}

void DualNetworkBoard::SetMl307Power(bool enabled) {
    if (ml307_power_en_pin_ == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(ml307_power_en_pin_, enabled ? 1 : 0);
}

bool DualNetworkBoard::ProbeMl307Presence(int delay_ms, int timeout_ms) {
    if (ml307_power_en_pin_ == GPIO_NUM_NC) {
        return false;
    }

    SetMl307Power(true);
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    for (int attempt = 1; attempt <= kInitialMl307DetectAttempts; ++attempt) {
        auto modem = AtModem::Detect(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_, 921600, timeout_ms, ml307_uart_num_);
        if (modem != nullptr) {
            ESP_LOGI(TAG, "Initial ML307 presence probe succeeded (attempt %d/%d)",
                     attempt, kInitialMl307DetectAttempts);
            return true;
        }
        ESP_LOGW(TAG, "Initial ML307 presence probe failed (attempt %d/%d)",
                 attempt, kInitialMl307DetectAttempts);
        if (attempt < kInitialMl307DetectAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kInitialMl307RetryDelayMs));
        }
    }
    return false;
}

void DualNetworkBoard::SelectInitialNetworkIfNeeded() {
    if (auto_select_done_) {
        return;
    }

    auto display = GetDisplay();
    if (display != nullptr) {
        display->SetStatus(Lang::Strings::CONNECTING);
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }

    const bool available = ProbeMl307Presence(kInitialMl307PowerSettleMs, kInitialMl307DetectTimeoutMs);
    ml307_available_ = available;
    network_type_ = available ? NetworkType::ML307 : NetworkType::WIFI;

    {
        Settings settings("network", true);
        settings.SetBool("ml307_available", available);
        settings.SetBool("auto_done", true);
    }
    SaveNetworkTypeToSettings(network_type_);
    auto_select_done_ = true;

    if (!available) {
        SetMl307Power(false);
    }
    InitializeCurrentBoard();
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();
    if (network_type_ == NetworkType::WIFI) {    
        SaveNetworkTypeToSettings(NetworkType::ML307);
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

void DualNetworkBoard::SetNetworkType(NetworkType type) {
    if (type == NetworkType::ML307 && !ml307_available_) {
        auto display = GetDisplay();
        if (display != nullptr) {
            display->ShowNotification("未检测到4G模块");
        }
        return;
    }
    if (network_type_ == type) {
        auto display = GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(type == NetworkType::ML307 ? "当前已是4G" : "当前已是WIFI");
        }
        return;
    }
    SaveNetworkTypeToSettings(type);
    auto display = GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(type == NetworkType::ML307 ? Lang::Strings::SWITCH_TO_4G_NETWORK
                                                             : Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    Application::GetInstance().Reboot();
}

void DualNetworkBoard::EnterWifiConfigMode() {
    auto* wifi_board = dynamic_cast<WifiBoard*>(current_board_.get());
    if (wifi_board != nullptr) {
        wifi_board->ResetWifiConfiguration();
        return;
    }

    ESP_LOGW(TAG, "EnterWifiConfigMode requested while current network is ML307, switching to WiFi");
    SaveNetworkTypeToSettings(NetworkType::WIFI);
    Application::GetInstance().Reboot();
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    SelectInitialNetworkIfNeeded();
    
    if (network_type_ == NetworkType::WIFI) {
        display->SetStatus(Lang::Strings::CONNECTING);
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
