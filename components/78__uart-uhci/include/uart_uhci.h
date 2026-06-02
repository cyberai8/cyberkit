#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <esp_err.h>
#include <driver/uart.h>

/**
 * NOTE:
 * This is a **minimal shim** implementation of the `uart-uhci` API used by `78__esp-ml307`.
 * It is not a real UHCI DMA driver. It provides a compatible interface and a simple RX worker
 * so the project can build and run in environments where the upstream component is unavailable.
 */
class UartUhci {
public:
    struct RxBuffer {
        uint8_t* data = nullptr;
        size_t capacity = 0;
    };

    struct RxPoolConfig {
        int buffer_count = 0;
        int buffer_size = 0;
    };

    struct Config {
        uart_port_t uart_port = UART_NUM_1;
        int dma_burst_size = 32;
        RxPoolConfig rx_pool;
    };

    struct RxEventData {
        RxBuffer* buffer = nullptr;
        size_t recv_size = 0;
    };

    using RxCallback = bool (*)(const RxEventData& data, void* user_data);
    using OverflowCallback = bool (*)(void* user_data);

    UartUhci() = default;
    ~UartUhci() = default;

    esp_err_t Init(const Config& cfg);
    void Deinit();

    void SetRxCallback(RxCallback cb, void* user_data);
    void SetOverflowCallback(OverflowCallback cb, void* user_data);

    esp_err_t StartReceive();
    void ReturnBuffer(RxBuffer* buffer);
    esp_err_t Transmit(const uint8_t* data, size_t len);

private:
    Config cfg_ = {};
    RxCallback rx_cb_ = nullptr;
    OverflowCallback overflow_cb_ = nullptr;
    void* cb_user_data_ = nullptr;
};

