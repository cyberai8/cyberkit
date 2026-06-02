#ifndef CST816S_TOUCH_H
#define CST816S_TOUCH_H

#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_lcd_touch.h>
#include <esp_err.h>

/**
 * @brief Wrapper class for CST816S touch controller initialization
 */
class Cst816sTouch {
public:
    Cst816sTouch(i2c_master_bus_handle_t i2c_bus)
        : i2c_bus_(i2c_bus),
          handle_(nullptr)
    {
    }

    ~Cst816sTouch()
    {
        // Handle cleanup if needed
        handle_ = nullptr;
    }

    /**
     * @brief Initialize CST816S touch controller
     *
     * @param width Touch panel width
     * @param height Touch panel height
     * @param swap_xy Swap X and Y coordinates
     * @param mirror_x Mirror X axis
     * @param mirror_y Mirror Y axis
     * @return bool true on success
     */
    bool init(int width,
              int height,
              bool swap_xy,
              bool mirror_x,
              bool mirror_y);

    esp_lcd_touch_handle_t get_handle() const
    {
        return handle_;
    }

private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_lcd_touch_handle_t handle_;
};

#endif // CST816S_TOUCH_H
