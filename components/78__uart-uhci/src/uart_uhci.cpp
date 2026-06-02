#include "uart_uhci.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>

namespace {
struct ShimState {
    UartUhci::Config cfg;
    UartUhci::RxCallback rx_cb = nullptr;
    UartUhci::OverflowCallback ov_cb = nullptr;
    void* user = nullptr;
    TaskHandle_t rx_task = nullptr;
    bool running = false;

    // single shared buffer; higher layers return it via ReturnBuffer()
    UartUhci::RxBuffer buffer;
    uint8_t* storage = nullptr;
};

static ShimState* g_state_for_port[UART_NUM_MAX] = {};

static void rx_task_fn(void* arg) {
    auto* st = static_cast<ShimState*>(arg);
    const TickType_t delay = pdMS_TO_TICKS(10);
    while (st->running) {
        if (!st->rx_cb || !st->storage) {
            vTaskDelay(delay);
            continue;
        }

        int rd = uart_read_bytes(st->cfg.uart_port, st->storage, st->buffer.capacity, pdMS_TO_TICKS(20));
        if (rd > 0) {
            UartUhci::RxEventData ev;
            ev.buffer = &st->buffer;
            ev.recv_size = static_cast<size_t>(rd);
            st->rx_cb(ev, st->user);
        } else {
            vTaskDelay(delay);
        }
    }
    st->rx_task = nullptr;
    vTaskDelete(nullptr);
}
} // namespace

esp_err_t UartUhci::Init(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // Create (or reuse) shim state for this UART port.
    if (g_state_for_port[cfg_.uart_port] == nullptr) {
        g_state_for_port[cfg_.uart_port] = new ShimState();
    }
    auto* st = g_state_for_port[cfg_.uart_port];
    st->cfg = cfg_;

    // Install UART driver with a small RX buffer; we will pull bytes in a task.
    // Use a modest buffer since higher layer does its own buffering.
    const int rx_buf_size = cfg_.rx_pool.buffer_size > 0 ? cfg_.rx_pool.buffer_size * 2 : 2048;
    const int tx_buf_size = 0;
    esp_err_t err = uart_driver_install(cfg_.uart_port, rx_buf_size, tx_buf_size, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // Allocate single buffer storage (best-effort; no real pool).
    if (st->storage == nullptr) {
        const size_t cap = cfg_.rx_pool.buffer_size > 0 ? static_cast<size_t>(cfg_.rx_pool.buffer_size) : 512;
        st->storage = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_8BIT));
        if (!st->storage) {
            return ESP_ERR_NO_MEM;
        }
        st->buffer.data = st->storage;
        st->buffer.capacity = cap;
    }

    return ESP_OK;
}

void UartUhci::Deinit() {
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) return;
    auto* st = g_state_for_port[cfg_.uart_port];
    if (!st) return;

    st->running = false;
    st->rx_cb = nullptr;
    st->ov_cb = nullptr;
    st->user = nullptr;

    auto* rx_task = st->rx_task;
    if (rx_task != nullptr && rx_task != xTaskGetCurrentTaskHandle()) {
        for (int i = 0; i < 50 && st->rx_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    uart_driver_delete(cfg_.uart_port);
}

void UartUhci::SetRxCallback(RxCallback cb, void* user_data) {
    rx_cb_ = cb;
    cb_user_data_ = user_data;
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) return;
    auto* st = g_state_for_port[cfg_.uart_port];
    if (!st) return;
    st->rx_cb = cb;
    st->user = user_data;
}

void UartUhci::SetOverflowCallback(OverflowCallback cb, void* user_data) {
    overflow_cb_ = cb;
    cb_user_data_ = user_data;
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) return;
    auto* st = g_state_for_port[cfg_.uart_port];
    if (!st) return;
    st->ov_cb = cb;
    st->user = user_data;
}

esp_err_t UartUhci::StartReceive() {
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) return ESP_ERR_INVALID_STATE;
    auto* st = g_state_for_port[cfg_.uart_port];
    if (!st) return ESP_ERR_INVALID_STATE;
    if (st->rx_task != nullptr) return ESP_OK;

    st->running = true;
    xTaskCreate(rx_task_fn, "uart_uhci_shim_rx", 3072, st, configMAX_PRIORITIES - 2, &st->rx_task);
    return ESP_OK;
}

void UartUhci::ReturnBuffer(RxBuffer* /*buffer*/) {
    // No-op for shim: we only use a single buffer.
}

esp_err_t UartUhci::Transmit(const uint8_t* data, size_t len) {
    if (!data || len == 0) return ESP_OK;
    if (cfg_.uart_port < 0 || cfg_.uart_port >= UART_NUM_MAX) return ESP_ERR_INVALID_STATE;
    int written = uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(data), len);
    return written < 0 ? ESP_FAIL : ESP_OK;
}
