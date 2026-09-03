#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>
#include <driver/gpio.h>

static const char *TAG = "DualNetworkBoard";

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
    }

    ConfigureMl307PowerPin();

    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); // 默认使用 WiFi (0)
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
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
