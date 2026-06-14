#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <esp_lv_adapter.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "gfx.h"
#include "cyber_base_control.h"

#include "application.h"
#include "libs/qrcode/lv_qrcode.h"
#ifdef CONFIG_BOARD_TYPE_CYBERVOC_V2_0
#include "boards/CyberVoc-Board-V2_0/ui_bridge.h"
#else
#include "boards/CyberVoc-Board-V1_2/ui_bridge.h"
#endif

#ifdef CONFIG_LANGUAGE_EN_US
#include "boards/CyberVoc-Board-V1_2-EN/assets/power/power.h"
#include "boards/CyberVoc-Board-V1_2-EN/assets/power/charge.h"
#include "boards/CyberVoc-Board-V1_2-EN/assets/number_14/number_14.h"
#include "boards/CyberVoc-Board-V1_2-EN/audio_analysis.h"
#include "boards/CyberVoc-Board-V1_2-EN/CyberVoc.h"
#elifdef CONFIG_LANGUAGE_RU_RU
#include "boards/CyberVoc-Board-V1_2-RU/assets/power/power.h"
#include "boards/CyberVoc-Board-V1_2-RU/assets/power/charge.h"
#include "boards/CyberVoc-Board-V1_2-RU/assets/number_14/number_14.h"
#include "boards/CyberVoc-Board-V1_2-RU/audio_analysis.h"
#include "boards/CyberVoc-Board-V1_2-RU/CyberVoc.h"
#elifdef CONFIG_LANGUAGE_ZH_CN
  #ifdef CONFIG_BOARD_TYPE_CYBERVOC_V2_0
  #include "boards/CyberVoc-Board-V2_0/assets/power/power.h"
  #include "boards/CyberVoc-Board-V2_0/assets/power/charge.h"
  #include "boards/CyberVoc-Board-V2_0/assets/number_14/number_14.h"
  #include "boards/CyberVoc-Board-V2_0/audio_analysis.h"
  #include "boards/CyberVoc-Board-V2_0/CyberVoc.h"
  #else
  #include "boards/CyberVoc-Board-V1_2/assets/power/power.h"
  #include "boards/CyberVoc-Board-V1_2/assets/power/charge.h"
  #include "boards/CyberVoc-Board-V1_2/assets/number_14/number_14.h"
  #include "boards/CyberVoc-Board-V1_2/audio_analysis.h"
  #include "boards/CyberVoc-Board-V1_2/CyberVoc.h"
  #endif
#endif

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace emote
{

    // ============================================================================
    // Constants and Type Definitions
    // ============================================================================

    static const char *TAG = "EmoteDisplay";

// UI Element Names - Centralized Management
#define UI_ELEMENT_EYE_ANIM "eye_anim"
#define UI_ELEMENT_TOAST_LABEL "toast_label"
#define UI_ELEMENT_CLOCK_LABEL "clock_label"
#define UI_ELEMENT_LISTEN_ANIM "listen_anim"
#define UI_ELEMENT_STATUS_ICON "status_icon"
#define UI_ELEMENT_EMERG_DLG "emerg_dlg"

// Icon Names - Centralized Management
#define ICON_MIC "icon_mic"
#define ICON_BATTERY "icon_tips"
#define ICON_SPEAKER_ZZZ "icon_speaker"
#define ICON_WIFI_FAILED "icon_WiFi_failed"
#define ICON_WIFI_OK "icon_wifi"
#define ICON_LISTEN "listen"

    using FlushIoReadyCallback = std::function<bool(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)>;
    using FlushCallback = std::function<void(gfx_handle_t, int, int, int, int, const void *)>;

    // ============================================================================
    // Global Variables
    // ============================================================================

    // UI element management
    static gfx_obj_t *g_obj_label_toast = nullptr;
    static gfx_obj_t *g_obj_label_clock = nullptr;
    static gfx_obj_t *g_obj_anim_eye = nullptr;
    static gfx_obj_t *g_obj_anim_listen = nullptr;
    static gfx_obj_t *g_obj_img_status = nullptr;
    static gfx_obj_t *g_obj_anim_emerg_dlg = nullptr;
    static lv_obj_t *g_power_overlay_container = nullptr;
    static lv_obj_t *g_power_overlay_label = nullptr;

    // Track current icon to determine when to show time
    static std::string g_current_icon_type = ICON_WIFI_FAILED;
    static gfx_image_dsc_t g_icon_img_dsc;

    // 电量框
    static gfx_obj_t *g_obj_battery = nullptr;
    static gfx_obj_t *g_obj_battery_number = nullptr; // 数字显示
    static gfx_obj_t *g_obj_charge = nullptr;
    // 低电量提醒
    static gfx_obj_t *low_battery_popup_ = nullptr;
    // ============================================================================
    // Forward Declarations
    // ============================================================================

    class EmoteDisplay;
    class EmoteEngine;

    enum class UIDisplayMode : uint8_t
    {
        SHOW_LISTENING = 1, // Show g_obj_anim_listen
        SHOW_TIME = 2,      // Show g_obj_label_clock
        SHOW_TIPS = 3       // Show g_obj_label_toast
    };

    // ============================================================================
    // Helper Functions
    // ============================================================================

    // Function to convert align string to GFX_ALIGN enum value
    char StringToGfxAlign(const std::string &align_str)
    {
        static const std::unordered_map<std::string, char> align_map = {
            {"GFX_ALIGN_DEFAULT", GFX_ALIGN_DEFAULT},
            {"GFX_ALIGN_TOP_LEFT", GFX_ALIGN_TOP_LEFT},
            {"GFX_ALIGN_TOP_MID", GFX_ALIGN_TOP_MID},
            {"GFX_ALIGN_TOP_RIGHT", GFX_ALIGN_TOP_RIGHT},
            {"GFX_ALIGN_LEFT_MID", GFX_ALIGN_LEFT_MID},
            {"GFX_ALIGN_CENTER", GFX_ALIGN_CENTER},
            {"GFX_ALIGN_RIGHT_MID", GFX_ALIGN_RIGHT_MID},
            {"GFX_ALIGN_BOTTOM_LEFT", GFX_ALIGN_BOTTOM_LEFT},
            {"GFX_ALIGN_BOTTOM_MID", GFX_ALIGN_BOTTOM_MID},
            {"GFX_ALIGN_BOTTOM_RIGHT", GFX_ALIGN_BOTTOM_RIGHT},
            {"GFX_ALIGN_OUT_TOP_LEFT", GFX_ALIGN_OUT_TOP_LEFT},
            {"GFX_ALIGN_OUT_TOP_MID", GFX_ALIGN_OUT_TOP_MID},
            {"GFX_ALIGN_OUT_TOP_RIGHT", GFX_ALIGN_OUT_TOP_RIGHT},
            {"GFX_ALIGN_OUT_LEFT_TOP", GFX_ALIGN_OUT_LEFT_TOP},
            {"GFX_ALIGN_OUT_LEFT_MID", GFX_ALIGN_OUT_LEFT_MID},
            {"GFX_ALIGN_OUT_LEFT_BOTTOM", GFX_ALIGN_OUT_LEFT_BOTTOM},
            {"GFX_ALIGN_OUT_RIGHT_TOP", GFX_ALIGN_OUT_RIGHT_TOP},
            {"GFX_ALIGN_OUT_RIGHT_MID", GFX_ALIGN_OUT_RIGHT_MID},
            {"GFX_ALIGN_OUT_RIGHT_BOTTOM", GFX_ALIGN_OUT_RIGHT_BOTTOM},
            {"GFX_ALIGN_OUT_BOTTOM_LEFT", GFX_ALIGN_OUT_BOTTOM_LEFT},
            {"GFX_ALIGN_OUT_BOTTOM_MID", GFX_ALIGN_OUT_BOTTOM_MID},
            {"GFX_ALIGN_OUT_BOTTOM_RIGHT", GFX_ALIGN_OUT_BOTTOM_RIGHT}};

        const auto it = align_map.find(align_str);
        if (it != align_map.cend())
        {
            return it->second;
        }

        ESP_LOGW(TAG, "Unknown align string: %s, using GFX_ALIGN_DEFAULT", align_str.c_str());
        return GFX_ALIGN_DEFAULT;
    }

    // ============================================================================
    // EmoteEngine Class Declaration
    // ============================================================================

    class EmoteEngine
    {
    public:
        EmoteEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                    const int width, const int height, EmoteDisplay *const display);
        ~EmoteEngine();

        void SetEyes(const std::string &emoji_name, const bool repeat, const int fps, EmoteDisplay *const display);
        void SetIcon(const std::string &icon_name, EmoteDisplay *const display);

        void *GetEngineHandle() const
        {
            return engine_handle_;
        }

        // Dialog animation methods
        bool SetDialogAnim(const std::string &emoji_name, EmoteDisplay *const display);
        void *GetDialogTimer() const { return dialog_timer_; }
        void SetDialogTimer(void *timer) { dialog_timer_ = timer; }
        void ClearDialogTimer() { dialog_timer_ = nullptr; }
        std::string GetCurrentDialogEmoji() const { return current_dialog_emoji_; }
        void ClearCurrentDialogEmoji() { current_dialog_emoji_.clear(); }

        // Callback functions (public to be accessible from static helper functions)
        static void OnFlush(const gfx_handle_t handle, const int x_start, const int y_start, const int x_end, const int y_end, const void *const color_data);
        static void OnDialogTimer(void *user_ctx);

    private:
        gfx_handle_t engine_handle_;
        void *dialog_timer_ = nullptr;
        std::string current_dialog_emoji_; // 当前显示的 dialog emoji 名称
    };

    // ============================================================================
    // UI Management Functions
    // ============================================================================

    static void SetUIDisplayMode(const UIDisplayMode mode, EmoteDisplay *const display)
    {
        if (!display)
        {
            ESP_LOGE(TAG, "SetUIDisplayMode: display is nullptr");
            return;
        }

        gfx_obj_set_visible(g_obj_anim_listen, false);
        gfx_obj_set_visible(g_obj_label_clock, false);
        gfx_obj_set_visible(g_obj_label_toast, false);

        // Show the selected control
        switch (mode)
        {
        case UIDisplayMode::SHOW_LISTENING:
        {
            gfx_obj_set_visible(g_obj_anim_listen, true);
            const AssetData emoji_data = display->GetIconData(ICON_LISTEN);
            if (emoji_data.data)
            {
                gfx_anim_set_src(g_obj_anim_listen, emoji_data.data, emoji_data.size);
                gfx_anim_set_segment(g_obj_anim_listen, 0, 0xFFFF, 20, true);
                gfx_anim_start(g_obj_anim_listen);
            }
            break;
        }
        case UIDisplayMode::SHOW_TIME:
            gfx_obj_set_visible(g_obj_label_clock, true);
            break;
        case UIDisplayMode::SHOW_TIPS:
            gfx_obj_set_visible(g_obj_label_toast, true);
            break;
        }
    }

    // ============================================================================
    // Graphics Initialization Functions
    // ============================================================================

    static void InitializeGraphics(const esp_lcd_panel_handle_t panel, gfx_handle_t *const engine_handle,
                                   const int width, const int height)
    {
        if (!panel || !engine_handle)
        {
            ESP_LOGE(TAG, "InitializeGraphics: Invalid parameters");
            return;
        }

        gfx_core_config_t gfx_cfg = {
            .flush_cb = EmoteEngine::OnFlush,
            .user_data = panel,
            .flags = {
                .swap = true,
                .double_buffer = true,
                .buff_dma = false,
                .buff_spiram = true,
            },
            .h_res = static_cast<uint32_t>(width),
            .v_res = static_cast<uint32_t>(height),
            .fps = 30,
            .buffers = {
                .buf1 = nullptr,
                .buf2 = nullptr,
                .buf_pixels = static_cast<size_t>(width * 16),
            },
            .task = GFX_EMOTE_INIT_CONFIG()};

        gfx_cfg.task.task_stack_caps = MALLOC_CAP_DEFAULT;
        // Keep the graphics renderer off CPU0, where WiFi/LwIP and the main task run.
        gfx_cfg.task.task_affinity = 1;
        gfx_cfg.task.task_priority = 4;
        gfx_cfg.task.task_stack = 8 * 1024;

        *engine_handle = gfx_emote_init(&gfx_cfg);
    }

    static void SetupUI(const gfx_handle_t engine_handle, EmoteDisplay *const display)
    {
        if (!display)
        {
            ESP_LOGE(TAG, "SetupUI: display is nullptr");
            return;
        }

        gfx_emote_set_bg_color(engine_handle, GFX_COLOR_HEX(0x000000));

        g_obj_anim_eye = gfx_anim_create(engine_handle);
        gfx_obj_align(g_obj_anim_eye, GFX_ALIGN_LEFT_MID, 10, 30);
        gfx_anim_set_auto_mirror(g_obj_anim_eye, true);
        gfx_obj_set_visible(g_obj_anim_eye, false);

        g_obj_label_toast = gfx_label_create(engine_handle);
        gfx_obj_align(g_obj_label_toast, GFX_ALIGN_TOP_MID, 0, 20);
        gfx_obj_set_size(g_obj_label_toast, 200, 40);
        gfx_label_set_text(g_obj_label_toast, Lang::Strings::INITIALIZING);
        gfx_label_set_color(g_obj_label_toast, GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(g_obj_label_toast, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(g_obj_label_toast, GFX_LABEL_LONG_SCROLL);
        gfx_label_set_scroll_speed(g_obj_label_toast, 20);
        gfx_label_set_scroll_loop(g_obj_label_toast, true);
        gfx_label_set_font(g_obj_label_toast, (gfx_font_t)&BUILTIN_TEXT_FONT);

        g_obj_label_clock = gfx_label_create(engine_handle);
        gfx_obj_align(g_obj_label_clock, GFX_ALIGN_TOP_MID, 0, 15);
        gfx_obj_set_size(g_obj_label_clock, 200, 50);
        gfx_label_set_text(g_obj_label_clock, " ");
        gfx_label_set_color(g_obj_label_clock, GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(g_obj_label_clock, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_font(g_obj_label_clock, (gfx_font_t)&BUILTIN_TEXT_FONT);
        gfx_label_set_long_mode(g_obj_label_clock, GFX_LABEL_LONG_SCROLL);

        g_obj_anim_listen = gfx_anim_create(engine_handle);
        gfx_obj_align(g_obj_anim_listen, GFX_ALIGN_TOP_MID, 0, 5);
        gfx_anim_start(g_obj_anim_listen);
        gfx_obj_set_visible(g_obj_anim_listen, false);

        g_obj_img_status = gfx_img_create(engine_handle);
        gfx_obj_align(g_obj_img_status, GFX_ALIGN_TOP_MID, -120, 18);

        g_obj_anim_emerg_dlg = gfx_anim_create(engine_handle);
        gfx_obj_align(g_obj_anim_emerg_dlg, GFX_ALIGN_CENTER, 0, 0);
        gfx_obj_set_visible(g_obj_anim_emerg_dlg, false);

        g_obj_battery = gfx_img_create(engine_handle);
        gfx_obj_align(g_obj_battery, GFX_ALIGN_TOP_MID, 100, 40);
        gfx_img_set_src(g_obj_battery, (void *)&power);
        gfx_obj_set_visible(g_obj_battery, false);

        g_obj_battery_number = gfx_label_create(engine_handle);
        gfx_obj_align(g_obj_battery_number, GFX_ALIGN_TOP_MID, 99, 44);
        gfx_obj_set_size(g_obj_battery_number, 26, 16);
        gfx_label_set_font(g_obj_battery_number, (gfx_font_t)&number_14);
        gfx_label_set_color(g_obj_battery_number, GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(g_obj_battery_number, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_text(g_obj_battery_number, " ");

        g_obj_charge = gfx_img_create(engine_handle);
        gfx_obj_align(g_obj_charge, GFX_ALIGN_TOP_MID, 109, 41);
        gfx_img_set_src(g_obj_charge, (void *)&charge);
        gfx_obj_set_visible(g_obj_charge, false);

        // gfx_obj_set_visible(g_obj_battery, false);

        // 创建低电量弹窗对象（使用标签对象模拟）
        low_battery_popup_ = gfx_label_create(engine_handle);
        if (low_battery_popup_ != NULL)
        {
            // 设置弹窗尺寸 - 假设屏幕宽度为screen_width，字体行高从text_font获取
            uint16_t screen_width = 320; // 需要根据实际屏幕宽度设置
            uint16_t line_height = 20;   // 需要根据实际字体行高设置
            uint16_t popup_width = screen_width * 0.9;
            uint16_t popup_height = line_height * 2;

            gfx_obj_set_size(low_battery_popup_, popup_width, popup_height);

            // 对齐到屏幕底部中间，带偏移
            // 假设spacing(4)返回16像素，需要根据实际主题设置
            int16_t y_offset = -16;
            gfx_obj_align(low_battery_popup_, GFX_ALIGN_BOTTOM_MID, 0, y_offset);

            // 设置背景颜色和样式
            gfx_color_t low_battery_color;
            low_battery_color.full = 0xF800; // 红色 in RGB565
            gfx_label_set_bg_color(low_battery_popup_, low_battery_color);
            gfx_label_set_bg_enable(low_battery_popup_, true);

            // 设置圆角 - 假设spacing(4)返回16像素
            // 注意：GFX API中可能需要使用其他方式设置圆角，这里使用背景色模拟
            gfx_label_set_opa(low_battery_popup_, 255); // 完全不透明

            // 创建标签文本（直接使用创建的对象设置文本）

            gfx_label_set_text(low_battery_popup_, Lang::Strings::BATTERY_NEED_CHARGE);

            // 设置文本颜色
            gfx_color_t white_color;
            white_color.full = 0xFFFF;                            // 白色 (RGB565: 0xFFFF)
            gfx_label_set_color(low_battery_popup_, white_color); // 白色

            // 设置文本对齐方式
            gfx_label_set_text_align(low_battery_popup_, GFX_TEXT_ALIGN_CENTER);
            // 设置行间距来调整垂直位置
            // gfx_label_set_line_spacing(low_battery_popup_,line_height);

            // 设置长文本模式（如果需要）
            // gfx_label_set_long_mode(low_battery_popup_, GFX_LABEL_LONG_WRAP);

            gfx_label_set_font(low_battery_popup_, (gfx_font_t)&BUILTIN_TEXT_FONT);

            // 初始隐藏弹窗
            gfx_obj_set_visible(low_battery_popup_, false);
        }

        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, display);
    }

    // ============================================================================
    // EmoteEngine Class Implementation
    // ============================================================================

    EmoteEngine::EmoteEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                             const int width, const int height, EmoteDisplay *const display)
    {
        InitializeGraphics(panel, &engine_handle_, width, height);

        if (display)
        {
            gfx_emote_lock(engine_handle_);
            SetupUI(engine_handle_, display);
            gfx_emote_unlock(engine_handle_);
        }
    }

    EmoteEngine::~EmoteEngine()
    {
        if (engine_handle_)
        {
            if (dialog_timer_)
            {
                gfx_timer_delete(engine_handle_, dialog_timer_);
                dialog_timer_ = nullptr;
            }
            gfx_emote_deinit(engine_handle_);
            engine_handle_ = nullptr;
        }
    }

    void EmoteEngine::SetEyes(const std::string &emoji_name, const bool repeat, const int fps, EmoteDisplay *const display)
    {
        if (!engine_handle_)
        {
            ESP_LOGE(TAG, "SetEyes: engine_handle_ is nullptr");
            return;
        }

        if (!display)
        {
            ESP_LOGE(TAG, "SetEyes: display is nullptr");
            return;
        }

        const AssetData emoji_data = display->GetEmojiData(emoji_name);
        if (emoji_data.data)
        {
            DisplayLockGuard lock(display);
            gfx_anim_set_src(g_obj_anim_eye, emoji_data.data, emoji_data.size);
            gfx_anim_set_segment(g_obj_anim_eye, 0, 0xFFFF, fps, repeat);
            gfx_obj_set_visible(g_obj_anim_eye, true);
            gfx_anim_start(g_obj_anim_eye);
        }
        else
        {
            ESP_LOGW(TAG, "SetEyes: No emoji data found for %s", emoji_name.c_str());
        }
        // auto& board = static_cast<EspS3Cat&>(Board::GetInstance());
        // if(board.GetAudioAnalysisMode() == AudioAnalysisMode::BEAT_DETECTION)
        // {
        //     if (gfx_obj_get_visible(g_obj_anim_eye))
        //     {
        //         gfx_obj_set_visible(g_obj_anim_eye, false);
        //     }

        //     return;
        // }
        // else
        // {
        //     if (!gfx_obj_get_visible(g_obj_anim_eye))
        //     {
        //         gfx_obj_set_visible(g_obj_anim_eye, true);
        //     }
        // }
    }

    void EmoteEngine::SetIcon(const std::string &icon_name, EmoteDisplay *const display)
    {
        if (!engine_handle_)
        {
            ESP_LOGE(TAG, "SetIcon: engine_handle_ is nullptr");
            return;
        }

        if (!display)
        {
            ESP_LOGE(TAG, "SetIcon: display is nullptr");
            return;
        }

        const AssetData icon_data = display->GetIconData(icon_name);
        if (icon_data.data)
        {
            DisplayLockGuard lock(display);

            std::memcpy(&g_icon_img_dsc.header, icon_data.data, sizeof(gfx_image_header_t));
            g_icon_img_dsc.data = static_cast<const uint8_t *>(icon_data.data) + sizeof(gfx_image_header_t);
            g_icon_img_dsc.data_size = icon_data.size - sizeof(gfx_image_header_t);

            gfx_img_set_src(g_obj_img_status, &g_icon_img_dsc);
        }
        else
        {
            ESP_LOGW(TAG, "SetIcon: No icon data found for %s", icon_name.c_str());
        }
        g_current_icon_type = icon_name;
    }

    bool EmoteEngine::SetDialogAnim(const std::string &emoji_name, EmoteDisplay *const display)
    {
        if (!engine_handle_)
        {
            ESP_LOGE(TAG, "SetDialogAnim: engine_handle_ is nullptr");
            return false;
        }

        if (!display)
        {
            ESP_LOGE(TAG, "SetDialogAnim: display is nullptr");
            return false;
        }

        const AssetData emoji_data = display->GetEmojiData(emoji_name);
        if (!emoji_data.data)
        {
            ESP_LOGW(TAG, "SetDialogAnim: No emoji data found for %s", emoji_name.c_str());
            return false;
        }

        DisplayLockGuard lock(display);
        gfx_anim_set_src(g_obj_anim_emerg_dlg, emoji_data.data, emoji_data.size);
        gfx_anim_set_segment(g_obj_anim_emerg_dlg, 0, 0xFFFF, emoji_data.fps > 0 ? emoji_data.fps : 20, emoji_data.loop);
        gfx_obj_set_visible(g_obj_anim_emerg_dlg, true);
        gfx_anim_start(g_obj_anim_emerg_dlg);

        // Hide eye animation when showing dialog
        if (g_obj_anim_eye)
        {
            gfx_obj_set_visible(g_obj_anim_eye, false);
        }

        // 更新当前 dialog emoji 名称
        current_dialog_emoji_ = emoji_name;

        return true;
    }

    void EmoteEngine::OnDialogTimer(void *user_ctx)
    {
        if (!user_ctx)
        {
            return;
        }

        EmoteDisplay *display = static_cast<EmoteDisplay *>(user_ctx);
        if (display && display->GetEngine())
        {
            display->StopAnimDialog();
        }
    }

#if 1
    void EmoteEngine::OnFlush(const gfx_handle_t handle, const int x_start, const int y_start,
                              const int x_end, const int y_end, const void *const color_data)
    {
        lv_display_t *disp = lv_display_get_default();
        if (disp != nullptr)
        {
            bool state = esp_lv_adapter_get_dummy_draw_enabled(disp);
            if (state)
            {
                esp_lv_adapter_dummy_draw_blit(
                    disp, x_start, y_start, x_end, y_end, color_data, true);
                // gfx_emote_flush_ready(handle, true);
            }
        }
        gfx_emote_flush_ready(handle, true);
    }
#else
    void EmoteEngine::OnFlush(const gfx_handle_t handle, const int x_start, const int y_start,
                              const int x_end, const int y_end, const void *const color_data)
    {
        auto *const panel = static_cast<esp_lcd_panel_handle_t>(gfx_emote_get_user_data(handle));
        if (panel)
        {
            esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
        }
        gfx_emote_flush_ready(handle, true);
    }
#endif
    // ============================================================================
    // EmoteDisplay Class Implementation
    // ============================================================================

    EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                               const int width, const int height)
    {
        width_ = width;
        height_ = height;
        InitializeEngine(panel, panel_io, width, height);
    }

    EmoteDisplay::~EmoteDisplay() = default;

    void EmoteDisplay::SetEmotion(const char *const emotion)
    {
        if (!emotion)
        {
            ESP_LOGE(TAG, "SetEmotion: emotion is nullptr");
            return;
        }

        ESP_LOGI(TAG, "SetEmotion: %s", emotion);
        if (!engine_)
        {
            return;
        }
        
        if (!gfx_obj_get_visible(g_obj_anim_eye))
        {
            gfx_obj_set_visible(g_obj_anim_eye, true);
        }

        const AssetData emoji_data = GetEmojiData(emotion);
        bool repeat = emoji_data.loop;
        int fps = emoji_data.fps > 0 ? emoji_data.fps : 20;

        if (std::strcmp(emotion, "idle") == 0 || std::strcmp(emotion, "neutral") == 0)
        {
            repeat = false;
        }

        DisplayLockGuard lock(this);
        engine_->SetEyes(emotion, repeat, fps, this);
    }

    void EmoteDisplay::SetChatMessage(const char *const role, const char *const content)
    {
        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);
        if (content && strlen(content) > 0)
        {
            gfx_label_set_text(g_obj_label_toast, content);
            SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
        }
    }

    void EmoteDisplay::SetStatus(const char *const status)
    {
        if (!status)
        {
            ESP_LOGE(TAG, "SetStatus: status is nullptr");
            return;
        }

        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);

        if (std::strcmp(status, Lang::Strings::LISTENING) == 0)
        {
            SetUIDisplayMode(UIDisplayMode::SHOW_LISTENING, this);
            engine_->SetEyes("happy", true, 20, this);
            engine_->SetIcon(ICON_MIC, this);
        }
        else if (std::strcmp(status, Lang::Strings::STANDBY) == 0)
        {
            SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
            engine_->SetIcon(ICON_BATTERY, this);
        }
        else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0)
        {
            SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
            engine_->SetIcon(ICON_SPEAKER_ZZZ, this);
        }
        else if (std::strcmp(status, Lang::Strings::ERROR) == 0)
        {
            SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
            engine_->SetIcon(ICON_WIFI_FAILED, this);
        }
        if (!gfx_obj_get_visible(g_obj_battery) && Application::GetInstance().GetDeviceState() != kDeviceStateWifiConfiguring)
        {
            gfx_obj_set_visible(g_obj_battery, true);
        }

        if (std::strcmp(status, Lang::Strings::CONNECTING) != 0)
        {
            gfx_label_set_text(g_obj_label_toast, status);
        }
    }

    void EmoteDisplay::ShowNotification(const char *notification, int duration_ms)
    {
        if (!notification || !engine_)
        {
            return;
        }
        ESP_LOGI(TAG, "ShowNotification: %s", notification);

        DisplayLockGuard lock(this);
        gfx_label_set_text(g_obj_label_toast, notification);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
    }

    void EmoteDisplay::ShowSuccessScreen(const char *message)
    {
        if (!message)
        {
            return;
        }
        ESP_LOGI(TAG, "ShowSuccessScreen: %s", message);
        DisplayLockGuard lock(this);

        esp_lv_adapter_lock(-1);

        static lv_obj_t *success_container = nullptr;
        static lv_obj_t *success_label = nullptr;

        if (success_container == nullptr)
        {
            lv_obj_t *scr = lv_scr_act();
            success_container = lv_obj_create(scr);
            int w = width_ > 0 ? width_ : 360;
            int h = height_ > 0 ? height_ : 360;
            
            lv_obj_set_size(success_container, w, h);
            lv_obj_align(success_container, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_bg_color(success_container, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(success_container, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(success_container, 0, 0);
            lv_obj_set_style_pad_all(success_container, 0, 0);
            lv_obj_set_flex_flow(success_container, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(success_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            success_label = lv_label_create(success_container);
            lv_obj_set_width(success_label, w * 0.8);
            lv_label_set_long_mode(success_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(success_label, lv_color_white(), 0);
            lv_obj_set_style_text_align(success_label, LV_TEXT_ALIGN_CENTER, 0);
            
            if (text_font_)
            {
                lv_obj_set_style_text_font(success_label, text_font_->font(), 0);
            }
            else
            {
                lv_obj_set_style_text_font(success_label, &BUILTIN_TEXT_FONT, 0);
            }

            ui_bridge_register_page_with_cycle(UI_BRIDGE_PAGE_WIFI_SUCCESS, &success_container, false);
        }

        lv_label_set_text(success_label, message);
        
        esp_lv_adapter_unlock();

        ui_bridge_switch_page(UI_BRIDGE_PAGE_WIFI_SUCCESS);
    }

    bool EmoteDisplay::ShowBootAnimation()
    {
        const AssetData boot = GetEmojiData("boot");
        if (!boot.data || boot.size == 0)
        {
            ESP_LOGW(TAG, "boot animation asset not found");
            return false;
        }
        return InsertAnimDialog("boot", 2000);
    }

    void EmoteDisplay::ShowPowerOverlay(const char *message)
    {
        ESP_LOGI(TAG, "ShowPowerOverlay: %s", message ? message : "");
        if (!message)
        {
            return;
        }

        DisplayLockGuard lock(this);
        esp_lv_adapter_lock(-1);

        if (g_power_overlay_container == nullptr)
        {
            lv_obj_t *scr = lv_scr_act();
            const int w = width_ > 0 ? width_ : 360;
            const int h = height_ > 0 ? height_ : 360;

            g_power_overlay_container = lv_obj_create(scr);
            lv_obj_set_size(g_power_overlay_container, w, h);
            lv_obj_align(g_power_overlay_container, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_bg_color(g_power_overlay_container, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(g_power_overlay_container, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(g_power_overlay_container, 0, 0);
            lv_obj_set_style_pad_all(g_power_overlay_container, 0, 0);
            lv_obj_clear_flag(g_power_overlay_container, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(g_power_overlay_container, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(g_power_overlay_container, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            g_power_overlay_label = lv_label_create(g_power_overlay_container);
            lv_obj_set_width(g_power_overlay_label, w * 0.85);
            lv_label_set_long_mode(g_power_overlay_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(g_power_overlay_label, lv_color_white(), 0);
            lv_obj_set_style_text_align(g_power_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
            if (text_font_)
            {
                lv_obj_set_style_text_font(g_power_overlay_label, text_font_->font(), 0);
            }
            else
            {
                lv_obj_set_style_text_font(g_power_overlay_label, &BUILTIN_TEXT_FONT, 0);
            }
        }

        lv_label_set_text(g_power_overlay_label, message);
        lv_obj_clear_flag(g_power_overlay_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_power_overlay_container);

        esp_lv_adapter_unlock();
    }

    void EmoteDisplay::HidePowerOverlay()
    {
        esp_lv_adapter_lock(-1);
        if (g_power_overlay_container != nullptr)
        {
            lv_obj_add_flag(g_power_overlay_container, LV_OBJ_FLAG_HIDDEN);
        }
        esp_lv_adapter_unlock();
    }

    void EmoteDisplay::UpdateStatusBar(bool update_all)
    {
        if (!engine_)
        {
            return;
        }
        auto &board = Board::GetInstance();
        // Only display time when battery icon is shown
        DisplayLockGuard lock(this);
        auto &app = Application::GetInstance();

        if (app.GetDeviceState() == kDeviceStateIdle)
        {
            auto music = board.GetMusic();
            bool music_play = music->MusicPlaying();

            if (!songtotime)
            {
                // 更新时钟
                time_t now;
                struct tm timeinfo;
                time(&now);

                setenv("TZ", "GMT+0", 1);
                tzset();
                localtime_r(&now, &timeinfo);

                char time_str[6];
                snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

                DisplayLockGuard lock(this);
                gfx_label_set_text(g_obj_label_clock, time_str);
                gfx_obj_set_visible(g_obj_label_clock, true);
                SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
                if (music_play)
                {
                    songtotime = true;
                }
            }
            else
            {
                std::string song_name;
                if (!music->Get_song_name().empty() || !music->Get_artist_name().empty())
                {
                    song_name = "《" + music->Get_song_name() + "》 " + music->Get_artist_name();
                }
                else
                {
                    song_name = "音乐播放中...";
                }

                DisplayLockGuard lock(this);
                SetChatMessage("lyric", song_name.c_str()); // SetChatMessage("lyric", formatted_song_name.c_str());
                gfx_obj_set_visible(g_obj_label_clock, false);
                songtotime = false;
            }
        }
        // 更新电量
        int battery_level;
        bool charging, discharging;
        const char *icon = nullptr;
        if (board.GetBatteryLevel(battery_level, charging, discharging))
        {

            if (battery_level <= 5)
            {
                if (charging)
                {
                    // gfx_label_set_text(low_battery_popup_, "低电量,先充电");
                    btr_level = false;
                    show_low_battery_popup(btr_level);
                }
                else
                {
                    btr_level = !btr_level;
                    gfx_label_set_text(low_battery_popup_, Lang::Strings::BATTERY_NEED_CHARGE);
                    show_low_battery_popup(btr_level);
                }
            }

            if (charging)
            {
                if (!gfx_obj_get_visible(g_obj_charge))
                {
                    gfx_obj_set_visible(g_obj_charge, true);
                    gfx_obj_align(g_obj_battery_number, GFX_ALIGN_TOP_MID, 95, 44);
                }
            }
            else
            {
                if (gfx_obj_get_visible(g_obj_charge))
                {
                    gfx_obj_set_visible(g_obj_charge, false);
                    gfx_obj_align(g_obj_battery_number, GFX_ALIGN_TOP_MID, 99, 44);
                }
            }

            DisplayLockGuard lock(this);
            char battery_text[4];
            sprintf(battery_text, "%d", battery_level);
            // ESP_LOGI(TAG,"电量: %s\n", battery_text);
            gfx_label_set_text(g_obj_battery_number, battery_text);
            // if (g_obj_img_status!= nullptr && battery_icon_ != icon) {
            //     ESP_LOGI(TAG,"电量2: %d%%\n", battery_level);
            //     battery_icon_ = icon;
            //     engine_->SetIcon(battery_icon_, this);
            // }
        }
    }

    void EmoteDisplay::ShowQRCode(const char *url, const char *device_name)
    {
        ESP_LOGI(TAG, "ShowQRCode: %s (size: %dx%d), device: %s", url, width_, height_, device_name ? device_name : "null");
        DisplayLockGuard lock(this);

        esp_lv_adapter_lock(-1);

        static lv_obj_t *wifi_config_container = nullptr;
        static lv_obj_t *qrcode = nullptr;
        static lv_obj_t *device_label = nullptr;

        if (wifi_config_container == nullptr)
        {
            lv_obj_t *scr = lv_scr_act();
            wifi_config_container = lv_obj_create(scr);
            // Use fallback if width/height are still 0 for some reason
            int w = width_ > 0 ? width_ : 360;
            int h = height_ > 0 ? height_ : 360;
            
            lv_obj_set_size(wifi_config_container, w, h);
            lv_obj_align(wifi_config_container, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_bg_color(wifi_config_container, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(wifi_config_container, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(wifi_config_container, 0, 0);
            lv_obj_set_style_pad_all(wifi_config_container, 0, 0);
            lv_obj_set_flex_flow(wifi_config_container, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(wifi_config_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t *qr_label = lv_label_create(wifi_config_container);
            lv_obj_set_width(qr_label, w * 0.6);
            lv_label_set_long_mode(qr_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(qr_label, "请扫描二维码进行配网或者添加设备");
            lv_obj_set_style_text_color(qr_label, lv_color_white(), 0);
            lv_obj_set_style_text_align(qr_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_margin_bottom(qr_label, 8, 0);
            
            // Use builtin font if available, otherwise use LVGL default
            if (text_font_)
            {
                lv_obj_set_style_text_font(qr_label, text_font_->font(), 0);
            }
            else
            {
                lv_obj_set_style_text_font(qr_label, &BUILTIN_TEXT_FONT, 0);
            }

            // QR code module is not available in this project build; fall back to showing the URL as text.
            qrcode = lv_label_create(wifi_config_container);
            lv_obj_set_width(qrcode, w * 0.8);
            lv_label_set_long_mode(qrcode, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(qrcode, lv_color_white(), 0);
            lv_obj_set_style_text_align(qrcode, LV_TEXT_ALIGN_CENTER, 0);

            device_label = lv_label_create(wifi_config_container);
            lv_obj_set_style_text_color(device_label, lv_color_hex(0x888888), 0);
            lv_obj_set_style_margin_top(device_label, 8, 0);
            // Use a smaller font if possible, or just default
            lv_obj_set_style_text_font(device_label, &lv_font_montserrat_14, 0);

            ui_bridge_register_page_with_cycle(UI_BRIDGE_PAGE_WIFI_CONFIG, &wifi_config_container, false);
        }

        lv_label_set_text_fmt(qrcode, "配网链接:\n%s", url ? url : "");
        if (device_name) {
            lv_label_set_text_fmt(device_label, "%s", device_name);
        } else {
            lv_label_set_text(device_label, "");
        }
        
        esp_lv_adapter_unlock();

        ui_bridge_switch_page(UI_BRIDGE_PAGE_WIFI_CONFIG);
    }

    void EmoteDisplay::HideQRCode()
    {
        ESP_LOGI(TAG, "HideQRCode");
        DisplayLockGuard lock(this);

        esp_lv_adapter_lock(-1);

        static lv_obj_t *wifi_config_container = nullptr;

        if (wifi_config_container != nullptr)
        {
            lv_obj_add_flag(wifi_config_container, LV_OBJ_FLAG_HIDDEN);
        }

        esp_lv_adapter_unlock();

        // Switch back to home page
        ui_bridge_switch_page(UI_BRIDGE_PAGE_HOME);
    }

    void EmoteDisplay::SetPowerSaveMode(bool on)
    {
        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);
        ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
        if (on)
        {
            gfx_anim_stop(g_obj_anim_eye);
        }
        else
        {
            gfx_anim_start(g_obj_anim_eye);
        }
    }

    void EmoteDisplay::SetPreviewImage(const void *image)
    {
        if (image)
        {
            ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
            if (engine_)
            {
            }
        }
    }

    void EmoteDisplay::SetTheme(Theme *const theme)
    {
        ESP_LOGI(TAG, "SetTheme: %p", theme);
    }
    void EmoteDisplay::AddEmojiData(const std::string &name, const void *const data, const size_t size,
                                    uint8_t fps, bool loop, bool lack)
    {
        emoji_data_map_[name] = AssetData(data, size, fps, loop, lack);
        // ESP_LOGI(TAG, "Added emoji data: %s, size: %d, fps: %d, loop: %s, lack: %s",
        //          name.c_str(), size, fps, loop ? "true" : "false", lack ? "true" : "false");

        DisplayLockGuard lock(this);
        if (name == "happy")
        {
            engine_->SetEyes("happy", loop, fps > 0 ? fps : 20, this);
        }
    }

    void EmoteDisplay::AddIconData(const std::string &name, const void *const data, const size_t size)
    {
        icon_data_map_[name] = AssetData(data, size);
        ESP_LOGD(TAG, "Added icon data: %s, size: %d", name.c_str(), size);

        DisplayLockGuard lock(this);
        if (name == ICON_WIFI_FAILED)
        {
            SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
            engine_->SetIcon(ICON_WIFI_FAILED, this);
        }
    }

    void EmoteDisplay::AddLayoutData(const std::string &name, const std::string &align_str,
                                     const int x, const int y, const int width, const int height)
    {
        const char align_enum = StringToGfxAlign(align_str);
        ESP_LOGI(TAG, "layout: %-12s | %-20s(%d) | %4d, %4d | %4dx%-4d",
                 name.c_str(), align_str.c_str(), align_enum, x, y, width, height);

        struct UIElement
        {
            gfx_obj_t *obj;
            const char *name;
        };

        const UIElement elements[] = {
            {g_obj_anim_eye, UI_ELEMENT_EYE_ANIM},
            {g_obj_label_toast, UI_ELEMENT_TOAST_LABEL},
            {g_obj_label_clock, UI_ELEMENT_CLOCK_LABEL},
            {g_obj_anim_listen, UI_ELEMENT_LISTEN_ANIM},
            {g_obj_img_status, UI_ELEMENT_STATUS_ICON},
            {g_obj_anim_emerg_dlg, UI_ELEMENT_EMERG_DLG},
        };

        DisplayLockGuard lock(this);
        for (const auto &element : elements)
        {
            if (name == element.name && element.obj)
            {
                gfx_obj_align(element.obj, align_enum, x, y);
                if (width > 0 && height > 0)
                {
                    gfx_obj_set_size(element.obj, width, height);
                }
                return;
            }
        }

        ESP_LOGW(TAG, "AddLayoutData: UI element '%s' not found", name.c_str());
    }

    void EmoteDisplay::AddTextFont(std::shared_ptr<LvglFont> text_font)
    {
        if (!text_font)
        {
            ESP_LOGW(TAG, "AddTextFont: text_font is nullptr");
            return;
        }

        text_font_ = text_font;
        ESP_LOGD(TAG, "AddTextFont: Text font added successfully");

        DisplayLockGuard lock(this);
        if (g_obj_label_toast && text_font_)
        {
            gfx_label_set_font(g_obj_label_toast, const_cast<void *>(static_cast<const void *>(text_font_->font())));
        }
        if (g_obj_label_clock && text_font_)
        {
            gfx_label_set_font(g_obj_label_clock, const_cast<void *>(static_cast<const void *>(text_font_->font())));
        }
    }

    AssetData EmoteDisplay::GetEmojiData(const std::string &name) const
    {
        const auto it = emoji_data_map_.find(name);
        if (it != emoji_data_map_.cend())
        {
            return it->second;
        }
        return AssetData();
    }

    AssetData EmoteDisplay::GetIconData(const std::string &name) const
    {
        const auto it = icon_data_map_.find(name);
        if (it != icon_data_map_.cend())
        {
            return it->second;
        }
        return AssetData();
    }

    EmoteEngine *EmoteDisplay::GetEngine() const
    {
        return engine_.get();
    }

    void *EmoteDisplay::GetEngineHandle() const
    {
        return engine_ ? engine_->GetEngineHandle() : nullptr;
    }

    void EmoteDisplay::InitializeEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                                        const int width, const int height)
    {
        engine_ = std::make_unique<EmoteEngine>(panel, panel_io, width, height, this);
    }

    bool EmoteDisplay::Lock(const int timeout_ms)
    {
        if (engine_ && engine_->GetEngineHandle())
        {
            gfx_emote_lock(engine_->GetEngineHandle());
            return true;
        }
        return false;
    }

    void EmoteDisplay::Unlock()
    {
        if (engine_ && engine_->GetEngineHandle())
        {
            gfx_emote_unlock(engine_->GetEngineHandle());
        }
    }

    bool EmoteDisplay::StopAnimDialog()
    {
        if (!engine_)
        {
            return false;
        }

        void *engine_handle = engine_->GetEngineHandle();
        if (!engine_handle)
        {
            return false;
        }

        gfx_emote_lock(engine_handle);

        // Stop and delete timer if exists
        void *dialog_timer = engine_->GetDialogTimer();
        if (dialog_timer)
        {
            gfx_timer_delete(engine_handle, dialog_timer);
            engine_->ClearDialogTimer();
        }

        if (g_obj_anim_emerg_dlg)
        {
            gfx_obj_set_visible(g_obj_anim_emerg_dlg, false);
        }

        if (g_obj_anim_eye)
        {
            gfx_obj_set_visible(g_obj_anim_eye, true);
        }

        // 标记位置：清除当前 dialog emoji 名称（手动停止或自动停止时）
        engine_->ClearCurrentDialogEmoji();

        gfx_emote_unlock(engine_handle);

        return true;
    }

    bool EmoteDisplay::InsertAnimDialog(const char *emoji_name, uint32_t duration_ms)
    {
        if (!emoji_name || !engine_)
        {
            return false;
        }

        // 如果插入的 emoji 和当前显示的一样，不处理，直接返回
        if (engine_->GetCurrentDialogEmoji() == emoji_name)
        {
            ESP_LOGD(TAG, "InsertAnimDialog: Same emoji %s already displayed, skipping", emoji_name);
            return true;
        }

        void *engine_handle = engine_->GetEngineHandle();
        if (!engine_handle)
        {
            return false;
        }

        // Stop existing timer if any
        gfx_emote_lock(engine_handle);
        // ESP_LOGI(TAG, "开始进行吉他动画");
        if (gfx_obj_get_visible(g_obj_anim_eye))
        {
            // ESP_LOGI(TAG,"隐藏眼睛动画");
            gfx_obj_set_visible(g_obj_anim_eye, false);
        }
        void *dialog_timer = engine_->GetDialogTimer();
        if (dialog_timer)
        {
            gfx_timer_delete(engine_handle, dialog_timer);
            engine_->ClearDialogTimer();
        }
        gfx_emote_unlock(engine_handle);

        // Set dialog animation (this will lock internally)
        if (!engine_->SetDialogAnim(emoji_name, this))
        {
            return false;
        }

        // Create timer for auto-stop
        gfx_emote_lock(engine_handle);

        void *timer = gfx_timer_create(engine_handle, EmoteEngine::OnDialogTimer, duration_ms, this);
        if (!timer)
        {
            ESP_LOGE(TAG, "Failed to create dialog timer");
            gfx_emote_unlock(engine_handle);
            StopAnimDialog();
            return false;
        }

        gfx_timer_set_repeat_count(timer, 1); // Execute only once
        engine_->SetDialogTimer(timer);
        gfx_emote_unlock(engine_handle);

        return true;
    }

    void EmoteDisplay::RefreshAll()
    {
        if (!engine_)
        {
            ESP_LOGI(TAG, "Refresh all: engine_ is nullptr");
            return;
        }

        void *handle = engine_->GetEngineHandle();
        if (!handle)
        {
            ESP_LOGI(TAG, "Refresh all: handle is nullptr");
            return;
        }

        gfx_emote_refresh_all(handle);
    }

    void EmoteDisplay::PauseAnimationsForLvgl()
    {
        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);
        ESP_LOGI(TAG, "Pause emote animations for LVGL page");
        if (g_obj_anim_eye)
        {
            gfx_anim_stop(g_obj_anim_eye);
        }
        if (g_obj_anim_listen)
        {
            gfx_anim_stop(g_obj_anim_listen);
        }
        if (g_obj_anim_emerg_dlg)
        {
            gfx_anim_stop(g_obj_anim_emerg_dlg);
        }
    }

    void EmoteDisplay::ResumeAnimationsForEmote()
    {
        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);
        ESP_LOGI(TAG, "Resume emote animations for home page");
        if (g_obj_anim_eye && gfx_obj_get_visible(g_obj_anim_eye))
        {
            gfx_anim_start(g_obj_anim_eye);
        }
        if (g_obj_anim_listen && gfx_obj_get_visible(g_obj_anim_listen))
        {
            gfx_anim_start(g_obj_anim_listen);
        }
        if (g_obj_anim_emerg_dlg && gfx_obj_get_visible(g_obj_anim_emerg_dlg))
        {
            gfx_anim_start(g_obj_anim_emerg_dlg);
        }
    }

    // 显示低电量弹窗的函数
    void EmoteDisplay::show_low_battery_popup(bool show)
    {
        if (low_battery_popup_ != NULL)
        {
            DisplayLockGuard lock(this);
            gfx_obj_set_visible(low_battery_popup_, show);
        }
    }
    void EmoteDisplay::set_clock_text(const std::string &song_name)
    {
        if (!engine_)
        {
            return;
        }

        DisplayLockGuard lock(this);
        gfx_label_set_text(g_obj_label_clock, song_name.c_str());
    }

} // namespace emote
