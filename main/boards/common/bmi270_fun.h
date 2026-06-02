// bmi270_fun.h
#ifndef BMI270_FUN_H
#define BMI270_FUN_H

#include "bmi270.h"//姿态传感器
#include "../managed_components/espressif2022__bmi270/bmi270_examples/common/common.h"
#include "i2c_bus.h"
#include <functional>
#include <cmath>    // 对于 C++

#define SHAKE_DETECTOR_TAG "ShakeDetector"

class Bmi270_fun
{
private:
    i2c_bus_handle_t i2c_bus;
    struct bmi2_dev bmi2_dev;
    int8_t rslt;
    
    bool is_shaking_ = false;
    uint32_t last_shake_time_ = 0;
    uint32_t shake_count_ = 0;
    uint32_t last_detection_time_ = 0;

public:
    Bmi270_fun(i2c_bus_handle_t i2c_bus_);
    ~Bmi270_fun();

    // 初始化传感器
    bool Initialize();
    
    // 检测是否被剧烈甩动
    bool IsViolentShake();
    
    // 设置甩动检测严格度 (0-2: 宽松-严格)
    void SetStrictness(uint8_t level);

private:
    void setup_strict_shake_detection();
    bool check_acceleration_intensity();
};

#endif // BMI270_FUN_H