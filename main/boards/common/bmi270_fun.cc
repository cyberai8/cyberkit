#include "bmi270_fun.h"

Bmi270_fun::Bmi270_fun(i2c_bus_handle_t i2c_bus_) : i2c_bus(i2c_bus_)
{
}

Bmi270_fun::~Bmi270_fun()
{
}

bool Bmi270_fun::Initialize()
{
    rslt = bmi2_interface_init(&bmi2_dev, BMI2_I2C_INTF, 0x68, i2c_bus);
    if (rslt != BMI2_OK) {
        ESP_LOGE(SHAKE_DETECTOR_TAG, "接口初始化失败: %d", rslt);
        return false;
    }

    rslt = bmi270_init(&bmi2_dev);
    if (rslt != BMI2_OK) {
        ESP_LOGE(SHAKE_DETECTOR_TAG, "传感器初始化失败: %d", rslt);
        return false;
    }

    setup_strict_shake_detection();
    ESP_LOGI(SHAKE_DETECTOR_TAG, "BMI270 初始化成功 - 2G阈值甩动检测模式");
    return true;
}

void Bmi270_fun::setup_strict_shake_detection()
{
    // 启用加速度计和任意运动检测
    uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_ANY_MOTION };
    
    rslt = bmi270_sensor_enable(sens_list, 2, &bmi2_dev);
    if (rslt != BMI2_OK) {
        ESP_LOGE(SHAKE_DETECTOR_TAG, "启用传感器失败: %d", rslt);
        return;
    }

    // 配置加速度计为高性能模式，8G量程
    struct bmi2_sens_config accel_cfg;
    accel_cfg.type = BMI2_ACCEL;
    if (bmi270_get_sensor_config(&accel_cfg, 1, &bmi2_dev) == BMI2_OK) {
        accel_cfg.cfg.acc.odr = BMI2_ACC_ODR_100HZ;  // 100Hz采样率
        accel_cfg.cfg.acc.range = BMI2_ACC_RANGE_8G; // 8G量程
        accel_cfg.cfg.acc.bwp = BMI2_ACC_OSR4_AVG1;  // 高性能模式
        accel_cfg.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
        bmi270_set_sensor_config(&accel_cfg, 1, &bmi2_dev);
    }

    // 设置中等灵敏度的运动检测（主要靠强度验证）
    SetStrictness(1);

    // 配置中断
    struct bmi2_sens_int_config int_config = {
        .type = BMI2_ANY_MOTION,
        .hw_int_pin = BMI2_INT1
    };
    
    rslt = bmi270_map_feat_int(&int_config, 1, &bmi2_dev);
    if (rslt != BMI2_OK) {
        ESP_LOGE(SHAKE_DETECTOR_TAG, "配置中断失败: %d", rslt);
    }
}

void Bmi270_fun::SetStrictness(uint8_t level)
{
    struct bmi2_sens_config any_motion_cfg;
    any_motion_cfg.type = BMI2_ANY_MOTION;
    
    if (bmi270_get_sensor_config(&any_motion_cfg, 1, &bmi2_dev) != BMI2_OK) {
        return;
    }

    // 根据严格度级别调整参数
    switch (level) {
        case 0: // 宽松 - 检测一般晃动
            any_motion_cfg.cfg.any_motion.threshold = 0x20;  // 32mg
            any_motion_cfg.cfg.any_motion.duration = 0x03;   // 60ms
            break;
        case 1: // 中等 - 检测明显运动
            any_motion_cfg.cfg.any_motion.threshold = 0x40;  // 64mg
            any_motion_cfg.cfg.any_motion.duration = 0x04;   // 80ms
            break;
        case 2: // 严格 - 检测较强运动
            any_motion_cfg.cfg.any_motion.threshold = 0x60;  // 96mg
            any_motion_cfg.cfg.any_motion.duration = 0x06;   // 120ms
            break;
        default:
            any_motion_cfg.cfg.any_motion.threshold = 0x40;
            any_motion_cfg.cfg.any_motion.duration = 0x04;
            break;
    }
    
    any_motion_cfg.cfg.any_motion.select_x = BMI2_ENABLE;
    any_motion_cfg.cfg.any_motion.select_y = BMI2_ENABLE;
    any_motion_cfg.cfg.any_motion.select_z = BMI2_ENABLE;

    bmi270_set_sensor_config(&any_motion_cfg, 1, &bmi2_dev);
    
    const char* level_names[] = {"宽松", "中等", "严格"};
    ESP_LOGI(SHAKE_DETECTOR_TAG, "运动检测灵敏度设置为: %s", level_names[level]);
}

bool Bmi270_fun::IsViolentShake()
{
    uint16_t int_status = 0;
    rslt = bmi2_get_int_status(&int_status, &bmi2_dev);
    
    uint32_t current_time = esp_timer_get_time() / 1000;
    
    // 检查任意运动中断
    if (rslt == BMI2_OK && (int_status & BMI270_ANY_MOT_STATUS_MASK)) {
        // 检查加速度强度是否超过2G
        if (check_acceleration_intensity()) {
            // 防止频繁触发，设置最小间隔
            if (current_time - last_shake_time_ > 2000) { // 2秒冷却
                last_shake_time_ = current_time;
                is_shaking_ = true;
                return true;
            }
        }
    }
    
    is_shaking_ = false;
    return false;
}

bool Bmi270_fun::check_acceleration_intensity()
{
    // 读取原始加速度数据
    struct bmi2_sens_data sensor_data;
    rslt = bmi2_get_sensor_data(&sensor_data, &bmi2_dev);
    
    if (rslt != BMI2_OK) {
        return false;
    }
    
    // 计算加速度矢量幅度 (使用原始LSB值)
    int16_t acc_x = sensor_data.acc.x;
    int16_t acc_y = sensor_data.acc.y; 
    int16_t acc_z = sensor_data.acc.z;
    
    // 计算变化幅度 (绝对值)
    int32_t magnitude_sq = (int32_t)acc_x * acc_x + (int32_t)acc_y * acc_y + (int32_t)acc_z * acc_z;
    
    // 设置2G阈值
    // 在8G量程下，1G ≈ 4096 LSB，2G对应的平方值为: (2*4096)^2 = 67108864
    //4G： (4*4096)^2 = 268435456
    //6G： (6*4096)^2 = 603979776
    const int32_t G2_THRESHOLD_SQ = 67108864L; // 2G的平方值

    
    if (magnitude_sq > G2_THRESHOLD_SQ) {
        // 计算实际G值用于调试
        float g_value = sqrtf((float)magnitude_sq) / 4096.0f;
        ESP_LOGI(SHAKE_DETECTOR_TAG, "🎯 超过2G阈值: 强度 %.1fG, 原始值(%d, %d, %d)", 
                g_value, acc_x, acc_y, acc_z);
        return true;
    } else {
        // 调试信息：显示当前强度但未达到阈值
        float g_value = sqrtf((float)magnitude_sq) / 4096.0f;
        if (g_value > 4.0f) { // 只记录超过4G的运动
            ESP_LOGD(SHAKE_DETECTOR_TAG, "当前强度: %.1fG (未达4G阈值)", g_value);
        }
        return false;
    }
}