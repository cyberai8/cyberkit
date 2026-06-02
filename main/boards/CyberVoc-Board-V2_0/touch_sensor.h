/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include "esp_err.h"
#include "iot_button.h"

#if defined(__cplusplus)

class TouchSensor {
public:
    TouchSensor();
    ~TouchSensor();

    bool init(uint8_t pcb_verison);

    button_handle_t get_button_handle();
        //设置顶部触摸可以触发
    void set_touch(bool enable){use_touch = enable;}
    bool get_touch(){return use_touch;}

private:
    void (*_callback)(int);
    int _min;
    int _max;

    bool use_touch = false;//触摸默认关闭
};

#endif