#include "ui_bridge.h"
#include "board.h"
#include "wifi_board.h"
#include "display/emote_display.h"
#include "customer_ui/alarm_api.h"
#include "application.h"
#include <esp_log.h>
#include <lvgl.h>
#include <esp_lv_adapter.h>

#include <wifi_station.h>

#ifdef CONFIG_LANGUAGE_EN_US
#include "boards/CyberVoc-Board-V1_2-EN/CyberVoc.h"
#elifdef CONFIG_LANGUAGE_RU_RU
#include "boards/CyberVoc-Board-V1_2-RU/CyberVoc.h"
#elifdef CONFIG_LANGUAGE_ZH_CN
#include "boards/CyberVoc-Board-V1_2/CyberVoc.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "ui_bridge"
// 定义操作类型枚举
typedef enum
{
    NVS_OP_WIFI_RESET = 0,
    NVS_OP_SET_INT
} nvs_operation_t;

// 定义请求结构体
typedef struct
{
    nvs_operation_t operation;
    std::string ns_name;
    std::string key;
    int32_t value;
} nvs_write_request_t;

// 静态变量声明
static QueueHandle_t s_nvs_write_queue = NULL;
static TaskHandle_t s_nvs_write_task = NULL;

// NVS写任务
// NVS写任务函数
static void nvs_write_task_func(void *arg)
{
    ESP_LOGI("UI_BRIDGE", "NVS write task started");

    while (1)
    {
        nvs_write_request_t request;

        if (xQueueReceive(s_nvs_write_queue, &request, portMAX_DELAY))
        {
            ESP_LOGI("UI_BRIDGE", "Processing NVS operation: %d", request.operation);

            switch (request.operation)
            {
            case NVS_OP_WIFI_RESET:
            {
                ESP_LOGI("UI_BRIDGE", "Resetting WiFi configuration");

                {
                    Settings settings("wifi", true);
                    settings.SetInt("force_ap", 1);
                }

                auto &board = Board::GetInstance();
                auto *display = board.GetDisplay();
                if (display)
                {
                    display->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
                }

                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGI("UI_BRIDGE", "Rebooting device...");
                esp_restart();
                break;
            }

            case NVS_OP_SET_INT:
            {
                ESP_LOGI("UI_BRIDGE", "Setting NVS int: ns=%s, key=%s, value=%ld",
                         request.ns_name.c_str(), request.key.c_str(), request.value);

                Settings settings(request.ns_name.c_str(), true);
                settings.SetInt(request.key.c_str(), request.value);
                break;
            }

            default:
                ESP_LOGW("UI_BRIDGE", "Unknown NVS operation: %d", request.operation);
                break;
            }
        }
    }
}
/* Gesture detection state structure */
typedef struct
{
    bool active;
    bool handled;
    lv_coord_t start_x;
    lv_coord_t start_y;
    uint32_t press_start_time;
    bool long_press_reported;
} ui_bridge_gesture_state_t;

/* Gesture detection state */
static ui_bridge_gesture_state_t s_gesture_state = {
    .active = false,
    .handled = false,
    .start_x = 0,
    .start_y = 0,
    .press_start_time = 0,
    .long_press_reported = false,
};

/* Page management using linked list */
typedef struct ui_bridge_page_node
{
    const char *page_id;
    lv_obj_t **container;
    bool in_cycle; /* Whether this page should be included in cycle navigation */
    struct ui_bridge_page_node *next;
} ui_bridge_page_node_t;

static ui_bridge_page_node_t *s_page_list = NULL; /* Linked list head */
static const char *s_current_page = NULL;
static ui_bridge_page_switch_cb_t s_page_switch_cb = NULL;
static void *s_page_switch_user_data = NULL;

/* Base emote UI container */
static lv_obj_t *s_base_container = NULL;

/* Cached display pointer for emote refresh */
static emote::EmoteDisplay *s_cached_emote_display = nullptr;

/* Forward declarations */
static void ui_bridge_handle_gesture_navigation(ui_bridge_gesture_type_t gesture_type);
static void ui_bridge_refresh_emote_display(void);
static bool ui_bridge_check_gesture_start_position(ui_bridge_gesture_type_t gesture, lv_coord_t start_x, lv_coord_t start_y);

/**
 * @brief Check if gesture start position is valid for the given gesture direction
 *
 * Valid start positions:
 * - SWIPE_UP:   Bottom edge (y > DISPLAY_HEIGHT - EDGE_THRESHOLD) and center X (±CENTER_RANGE)
 * - SWIPE_DOWN: Top edge (y < EDGE_THRESHOLD) and center X (±CENTER_RANGE)
 * - SWIPE_LEFT: Right edge (x > DISPLAY_WIDTH - EDGE_THRESHOLD) and center Y (±CENTER_RANGE)
 * - SWIPE_RIGHT: Left edge (x < EDGE_THRESHOLD) and center Y (±CENTER_RANGE)
 */
static bool ui_bridge_check_gesture_start_position(ui_bridge_gesture_type_t gesture, lv_coord_t start_x, lv_coord_t start_y)
{
    switch (gesture)
    {
    case UI_BRIDGE_GESTURE_SWIPE_UP:
        /* Must start from bottom edge and center X */
        return (start_y > (DISPLAY_HEIGHT - UI_BRIDGE_EDGE_THRESHOLD)) &&
               (LV_ABS(start_x - UI_BRIDGE_CENTER_X) <= UI_BRIDGE_CENTER_RANGE);

    case UI_BRIDGE_GESTURE_SWIPE_DOWN:
        /* Must start from top edge and center X */
        return (start_y < UI_BRIDGE_EDGE_THRESHOLD) &&
               (LV_ABS(start_x - UI_BRIDGE_CENTER_X) <= UI_BRIDGE_CENTER_RANGE);

    case UI_BRIDGE_GESTURE_SWIPE_LEFT:
        /* Must start from right edge and center Y */
        return (start_x > (DISPLAY_WIDTH - UI_BRIDGE_EDGE_THRESHOLD)) &&
               (LV_ABS(start_y - UI_BRIDGE_CENTER_Y) <= UI_BRIDGE_CENTER_RANGE);

    case UI_BRIDGE_GESTURE_SWIPE_RIGHT:
        /* Must start from left edge and center Y */
        return (start_x < UI_BRIDGE_EDGE_THRESHOLD) &&
               (LV_ABS(start_y - UI_BRIDGE_CENTER_Y) <= UI_BRIDGE_CENTER_RANGE);

    default:
        return true; /* No position requirement for other gestures */
    }
}

/* Touch gesture event callback */
static void ui_bridge_gesture_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    ui_bridge_gesture_state_t *state = &s_gesture_state;

    switch (code)
    {
    case LV_EVENT_PRESSED:
        if (indev)
        {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            state->active = true;
            state->handled = false;
            state->start_x = p.x;
            state->start_y = p.y;
            state->press_start_time = lv_tick_get();
            ESP_LOGI(TAG, "press at (%ld, %ld)", (long)p.x, (long)p.y);
        }
        else
        {
            ESP_LOGW(TAG, "press but indev is NULL");
        }
        break;

    case LV_EVENT_PRESSING:
    {
        if (!state->active || state->handled || !indev)
        {
            break;
        }

        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_coord_t dx = p.x - state->start_x;
        lv_coord_t dy = p.y - state->start_y;

        /* Log movement for debugging */
        static lv_coord_t last_dx = 0, last_dy = 0;
        if (LV_ABS(dx - last_dx) > 5 || LV_ABS(dy - last_dy) > 5)
        {
            ESP_LOGI(TAG, "PRESSING: dx=%ld, dy=%ld, threshold=%d",
                     (long)dx, (long)dy, UI_BRIDGE_GESTURE_SWIPE_THRESHOLD);
            last_dx = dx;
            last_dy = dy;
        }

        bool dx_exceeds = LV_ABS(dx) >= UI_BRIDGE_GESTURE_SWIPE_THRESHOLD;
        bool dy_exceeds = LV_ABS(dy) >= UI_BRIDGE_GESTURE_SWIPE_THRESHOLD;

        if ((dx_exceeds && !dy_exceeds) || (!dx_exceeds && dy_exceeds))
        {
            ui_bridge_gesture_type_t gesture = UI_BRIDGE_GESTURE_NONE;

            /* Determine swipe direction based on dominant axis */
            if (LV_ABS(dx) > LV_ABS(dy))
            {
                /* Horizontal swipe */
                if (dx < 0)
                {
                    gesture = UI_BRIDGE_GESTURE_SWIPE_LEFT;
                }
                else
                {
                    gesture = UI_BRIDGE_GESTURE_SWIPE_RIGHT;
                }
            }
            else
            {
                /* Vertical swipe */
                if (dy < 0)
                {
                    gesture = UI_BRIDGE_GESTURE_SWIPE_UP;
                }
                else
                {
                    gesture = UI_BRIDGE_GESTURE_SWIPE_DOWN;
                }
            }

            if (gesture != UI_BRIDGE_GESTURE_NONE)
            {
                if (ui_bridge_check_gesture_start_position(gesture, state->start_x, state->start_y))
                {
                    ESP_LOGI(TAG, "swipe detected: %d (start: %ld, %ld)", gesture,
                             (long)state->start_x, (long)state->start_y);
                    // ui_bridge_handle_gesture_navigation(gesture);
                    state->handled = true;
                }
                else
                {
                    ESP_LOGI(TAG, "swipe gesture %d rejected: invalid start position (%ld, %ld)",
                             gesture, (long)state->start_x, (long)state->start_y);
                }
            }
        }
        else if (dx_exceeds && dy_exceeds)
        {
            ESP_LOGI(TAG, "Both axes exceed threshold (dx=%ld, dy=%ld) - treating as drag, not swipe",
                     (long)dx, (long)dy);
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
    {
        if (!state->active || !indev)
        {
            state->active = false;
            state->handled = false;
            break;
        }

        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_coord_t dx = p.x - state->start_x;
        lv_coord_t dy = p.y - state->start_y;
        uint32_t press_duration = lv_tick_elaps(state->press_start_time);

        ESP_LOGI(TAG, "release: dx=%ld, dy=%ld, duration=%lu ms, handled=%d",
                 (long)dx, (long)dy, press_duration, state->handled);

        /* Check for swipe gesture on release (if not already handled) */
        if (!state->handled)
        {
            /* Only recognize as swipe if one axis exceeds threshold while the other doesn't */
            /* If both exceed threshold, it's likely dragging (e.g., arc), not a swipe */
            bool dx_exceeds = LV_ABS(dx) >= UI_BRIDGE_GESTURE_SWIPE_THRESHOLD;
            bool dy_exceeds = LV_ABS(dy) >= UI_BRIDGE_GESTURE_SWIPE_THRESHOLD;

            if ((dx_exceeds && !dy_exceeds) || (!dx_exceeds && dy_exceeds))
            {
                ui_bridge_gesture_type_t gesture = UI_BRIDGE_GESTURE_NONE;

                /* Determine swipe direction based on dominant axis */
                if (LV_ABS(dx) > LV_ABS(dy))
                {
                    /* Horizontal swipe */
                    if (dx < 0)
                    {
                        gesture = UI_BRIDGE_GESTURE_SWIPE_LEFT;
                    }
                    else
                    {
                        gesture = UI_BRIDGE_GESTURE_SWIPE_RIGHT;
                    }
                }
                else
                {
                    /* Vertical swipe */
                    if (dy < 0)
                    {
                        gesture = UI_BRIDGE_GESTURE_SWIPE_UP;
                    }
                    else
                    {
                        gesture = UI_BRIDGE_GESTURE_SWIPE_DOWN;
                    }
                }

                if (gesture != UI_BRIDGE_GESTURE_NONE)
                {
                    if (ui_bridge_check_gesture_start_position(gesture, state->start_x, state->start_y))
                    {
                        ESP_LOGI(TAG, "swipe detected: %d (start: %ld, %ld)", gesture,
                                 (long)state->start_x, (long)state->start_y);
                        ui_bridge_handle_gesture_navigation(gesture);
                        state->handled = true;
                        state->long_press_reported = true;
                    }
                    else
                    {
                        ESP_LOGI(TAG, "swipe gesture %d rejected: invalid start position (%ld, %ld)",
                                 gesture, (long)state->start_x, (long)state->start_y);
                        ui_bridge_handle_gesture_navigation(gesture);
                        state->handled = true;
                        state->long_press_reported = true;
                    }
                }
            }
            else if (dx_exceeds && dy_exceeds)
            {
                ESP_LOGI(TAG, "Both axes exceed threshold (dx=%ld, dy=%ld) - treating as drag, not swipe",
                         (long)dx, (long)dy);
            }
            else
            {
                /* It's a press (not a swipe) */
                ui_bridge_gesture_type_t gesture;
                if (press_duration >= UI_BRIDGE_GESTURE_LONG_PRESS_TIME_MS)
                {
                    gesture = UI_BRIDGE_GESTURE_LONG_PRESS;
                }
                else
                {
                    gesture = UI_BRIDGE_GESTURE_SHORT_PRESS;
                }
                ESP_LOGI(TAG, "press detected: %d (duration: %lu ms)", gesture, press_duration);
            }
        }

        state->active = false;
        state->handled = false;
        break;
    }

    default:
        /* Ignore all other events (LV_EVENT_FLUSH_WAIT_START, LV_EVENT_VSYNC, etc.) */
        break;
    }
}

/* Internal function to refresh emote display */
static void ui_bridge_refresh_emote_display(void)
{
    if (s_cached_emote_display == nullptr)
    {
        Display *base_display = Board::GetInstance().GetDisplay();
        if (!base_display)
        {
            ESP_LOGI(TAG, "Refresh all: base_display is nullptr");
            return;
        }
        s_cached_emote_display = dynamic_cast<emote::EmoteDisplay *>(base_display);
        if (!s_cached_emote_display)
        {
            ESP_LOGI(TAG, "Refresh all: emote_display is nullptr");
            return;
        }
    }

    if (s_cached_emote_display)
    {
        s_cached_emote_display->RefreshAll();
    }
}

/* Internal function to handle gesture-based page navigation */
static void ui_bridge_handle_gesture_navigation(ui_bridge_gesture_type_t gesture_type)
{
    /* Map gesture to page direction */
    int direction = 0;
    const char *gesture_name = NULL;

    switch (gesture_type)
    {
    case UI_BRIDGE_GESTURE_SWIPE_LEFT:
    case UI_BRIDGE_GESTURE_SWIPE_DOWN:
        direction = -1; /* Previous page */
        gesture_name = (gesture_type == UI_BRIDGE_GESTURE_SWIPE_LEFT) ? "LEFT" : "DOWN";
        break;
    case UI_BRIDGE_GESTURE_SWIPE_RIGHT:
    case UI_BRIDGE_GESTURE_SWIPE_UP:
        direction = 1; /* Next page */
        gesture_name = (gesture_type == UI_BRIDGE_GESTURE_SWIPE_RIGHT) ? "RIGHT" : "UP";
        break;
    default:
        return;
    }

    /* Check if page containers are registered */
    if (s_page_list == NULL)
    {
        ESP_LOGW(TAG, "No page containers registered");
        return;
    }

    /* Count total pages (only those in cycle) and find current page index */
    unsigned int total_count = 0;
    int current_index = -1;
    ui_bridge_page_node_t *node = s_page_list;
    while (node != NULL)
    {
        if (node->in_cycle)
        {
            if (node->page_id == s_current_page)
            {
                current_index = (int)total_count;
            }
            total_count++;
        }
        node = node->next;
    }

    /* If current page is not in registered list or no cycle pages, ignore gesture */
    if (current_index < 0 || total_count == 0)
    {
        return;
    }

    /* Calculate next page index with round-robin */
    int next_index = (current_index + direction + (int)total_count) % (int)total_count;

    /* Find target page by index (only counting cycle pages) */
    const char *target_page = NULL;
    const char *current_name = "UNKNOWN";
    const char *next_name = "UNKNOWN";
    node = s_page_list;
    int index = 0;
    while (node != NULL)
    {
        if (node->in_cycle)
        {
            if (index == current_index)
            {
                current_name = node->page_id ? node->page_id : "UNKNOWN";
            }
            if (index == next_index)
            {
                target_page = node->page_id;
                next_name = node->page_id ? node->page_id : "UNKNOWN";
            }
            index++;
        }
        node = node->next;
    }

    ESP_LOGI(TAG, "SWIPE_%s: %s (%d) -> %s (%d)",
             gesture_name, current_name, current_index, next_name, next_index);
    auto &app = Application::GetInstance();
    app.SwitchToIdle();
    ESP_LOGI(TAG, "先切换到空闲态,再变化UI");

    /* Call custom callback if set */
    if (s_page_switch_cb && s_page_switch_cb(target_page, s_page_switch_user_data))
    {
        return;
    }

    // if (app.GetDeviceState() == kDeviceStateSpeaking && app.GetDeviceState() == kDeviceStateListening) {

    //}

    /* Default page switching */
    ui_bridge_switch_page(target_page);
}

static void ui_bridge_base_container_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "屏幕点击");
        if (ui_bridge_is_on_home_page())
        {
            auto &board = Board::GetInstance();
            auto wifi_board = static_cast<WifiBoard *>(&board);
            auto &app = Application::GetInstance();

            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiStation::GetInstance().IsConnected())
            {
                // 添加静态变量来记录点击次数和时间
                static int click_count = 0;
                static TickType_t last_click_time = 0;
                TickType_t current_time = xTaskGetTickCount();

                // 检查是否是新的点击序列（距离上次点击超过1秒）
                if ((current_time - last_click_time) > pdMS_TO_TICKS(1000))
                {
                    click_count = 1; // 重置为1，因为这是新序列的第一次点击
                }
                else
                {
                    click_count++; // 在1秒内，增加计数
                }

                // 更新最后点击时间
                last_click_time = current_time;

                ESP_LOGI("UI_BRIDGE", "点击次数: %d", click_count);

                // 当点击次数达到3次时，发送请求
                if (click_count >= 3)
                {
                    // 创建请求结构体
                    nvs_write_request_t request;
                    request.operation = NVS_OP_WIFI_RESET;

                    // 发送到队列
                    if (s_nvs_write_queue)
                    {
                        BaseType_t result = xQueueSend(s_nvs_write_queue, &request, pdMS_TO_TICKS(100));
                        if (result == pdPASS)
                        {
                            ESP_LOGI("UI_BRIDGE", "WiFi reset request queued successfully (3 clicks in 1 second)");

                            // 重置点击计数，避免重复触发
                            click_count = 0;
                        }
                        else
                        {
                            ESP_LOGW("UI_BRIDGE", "Failed to queue WiFi reset request");
                        }
                    }
                    else
                    {
                        ESP_LOGW("UI_BRIDGE", "NVS write queue not initialized");
                    }
                }
                // 注意：这里删除了原来的 xQueueSend(s_nvs_write_queue, &request, 0);
            }
            else
            {
                if (board.GetMusic()->MusicPlaying())
                {
                    board.GetMusic()->StopStreaming();
                }
                vTaskDelay(pdMS_TO_TICKS(150));
                board.GetDisplay()->set_clock_text(" ");
                app.ToggleChatState();
            }
        }
    }
}
/* Public API implementation */
void ui_bridge_init(Display *display)
{
    ESP_LOGI("UI_BRIDGE", "Initializing UI bridge");

    /* 初始化NVS写队列和任务 */
    s_nvs_write_queue = xQueueCreate(10, sizeof(nvs_write_request_t));
    if (s_nvs_write_queue == NULL)
    {
        ESP_LOGE("UI_BRIDGE", "Failed to create NVS write queue");
    }
    else
    {
        ESP_LOGI("UI_BRIDGE", "NVS write queue created");
    }

    BaseType_t task_result = xTaskCreate(
        nvs_write_task_func,
        "nvs_write_task",
        4096,
        NULL,
        5,
        &s_nvs_write_task);

    if (task_result != pdPASS)
    {
        ESP_LOGE("UI_BRIDGE", "Failed to create NVS write task");
    }
    else
    {
        ESP_LOGI("UI_BRIDGE", "NVS write task created");
    }
    /* Cache display pointer */
    if (display)
    {
        s_cached_emote_display = dynamic_cast<emote::EmoteDisplay *>(display);
        if (s_cached_emote_display)
        {
            ESP_LOGI(TAG, "Cached emote display pointer: %p", (void *)s_cached_emote_display);
        }
    }

    /* Create base emote UI container */
    lv_obj_t *scr = lv_scr_act();
    s_base_container = lv_obj_create(scr);
    lv_obj_remove_style_all(s_base_container);
    lv_obj_set_size(s_base_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_align(s_base_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_base_container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_base_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_base_container, LV_OBJ_FLAG_CLICKABLE);

    /* Register base container as default page */
    ui_bridge_register_page(UI_BRIDGE_PAGE_HOME, &s_base_container);
    ui_bridge_switch_page(UI_BRIDGE_PAGE_HOME); /* Set as default page */
    lv_obj_add_event_cb(s_base_container, ui_bridge_base_container_event_cb, LV_EVENT_ALL, NULL);

    /* Create main UI (which will register its own pages) */
    alarm_create_ui();

    ESP_LOGI(TAG, "LVGL display bridge initialized for %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void ui_bridge_attach_gesture_handler(lv_indev_t *indev)
{
    ESP_LOGI(TAG, "Attaching gesture handler to input device: %p", (void *)indev);
    lv_indev_add_event_cb(indev, ui_bridge_gesture_event_cb, LV_EVENT_ALL, NULL);
}

bool ui_bridge_register_page(const char *page_id, lv_obj_t **container)
{
    return ui_bridge_register_page_with_cycle(page_id, container, true);
}

bool ui_bridge_register_page_with_cycle(const char *page_id, lv_obj_t **container, bool in_cycle)
{
    if (page_id == NULL)
    {
        ESP_LOGE(TAG, "Page ID cannot be NULL");
        return false;
    }

    /* Check if already registered */
    ui_bridge_page_node_t *node = s_page_list;
    while (node != NULL)
    {
        if (node->page_id != NULL && strcmp(node->page_id, page_id) == 0)
        {
            ESP_LOGW(TAG, "Page container '%s' already registered, updating", page_id);
            node->container = container;
            node->in_cycle = in_cycle;
            return true;
        }
        node = node->next;
    }

    /* Create new node */
    ui_bridge_page_node_t *new_node = (ui_bridge_page_node_t *)malloc(sizeof(ui_bridge_page_node_t));
    if (new_node == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for page container node");
        return false;
    }

    new_node->page_id = page_id;
    new_node->container = container;
    new_node->in_cycle = in_cycle;
    new_node->next = s_page_list; /* Insert at head */
    s_page_list = new_node;

    /* Count nodes for logging */
    unsigned int count = 0;
    node = s_page_list;
    while (node != NULL)
    {
        count++;
        node = node->next;
    }
    ESP_LOGD(TAG, "Registered page: %s (in_cycle: %s, total: %u)",
             page_id ? page_id : "UNKNOWN", in_cycle ? "true" : "false", count);
    return true;
}

void ui_bridge_switch_page(const char *page_id)
{
    if (page_id == NULL)
    {
        ESP_LOGW(TAG, "Cannot switch to NULL page");
        return;
    }
    auto &app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking && app.GetDeviceState() == kDeviceStateListening)
    {
        // ESP_LOGI(TAG, "先切换到空闲态,再变化UI");
        app.SwitchToIdle();
    }

    /* Update current page state */
    s_current_page = page_id;

    esp_lv_adapter_lock(-1);

    /* Set dummy draw mode for home page */
    lv_display_t *disp = lv_display_get_default();
    bool enable_dummy = (strcmp(page_id, UI_BRIDGE_PAGE_HOME) == 0);
    if (disp != nullptr)
    {
        esp_lv_adapter_set_dummy_draw(disp, enable_dummy);
    }

    /* Control visibility of all registered containers */
    ui_bridge_page_node_t *node = s_page_list;
    while (node != NULL)
    {
        lv_obj_t *container = *node->container;
        if (container != NULL)
        {
            if (node->page_id != NULL && strcmp(node->page_id, page_id) == 0)
            {
                lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
            }
        }
        node = node->next;
    }

    esp_lv_adapter_unlock();

    /* Refresh emote display if switching to home page */
    if (enable_dummy)
    {
        ui_bridge_refresh_emote_display();
        app.ToggleChatState();
    }
}

const char *ui_bridge_get_current_page(void)
{
    return s_current_page;
}

void ui_bridge_set_page_switch_callback(ui_bridge_page_switch_cb_t cb, void *user_data)
{
    s_page_switch_cb = cb;
    s_page_switch_user_data = user_data;
}

bool ui_bridge_is_on_home_page(void)
{
    return (s_current_page != NULL) && (strcmp(s_current_page, UI_BRIDGE_PAGE_HOME) == 0);
}
