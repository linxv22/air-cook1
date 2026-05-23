#include "app_events.h"
#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
#include "WIFI.h"
#include "ui_con.h"

#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_task";
extern _lock_t lvgl_api_lock;
esp_event_loop_handle_t loop_handle;
ESP_EVENT_DEFINE_BASE(AIR_COOKER_EVENTS);



// 工作状态，当前时间，当前温度，风扇转速，剩余时间，联网状态（顶层右上角WiFI小标志），语音识别的界面（识别到主人说话了就弹出，涉及到LVGL加字库）

WIFI_state_t WIFI_STATE = WIFI_STATE_INIT;

// ================= 事件中枢 (统一处理底层控制逻辑) =================
void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
    switch (id) {
        case EVENT_CMD_aircook: {
           cook_config_t *cfg = (cook_config_t *)event_data;
            aircook_start(cfg);
            ESP_LOGI(TAG, "Logic: Cook Started! Temp: %.1f C, Time: %ld s, Fan Enum: %d", 
                     cfg->temperature, cfg->time_s, cfg->fan_speed);
            break;
        }
        case EVENT_CMD_STOP: {
           aircook_stop();
           ESP_LOGI(TAG, "Logic: Cook Stopped!");
            break;
        }
        case EVENT_CMD_SET_TEMP: {
            float *temp = (float *)event_data;
            aircook_set_tem(*temp);
            ESP_LOGI(TAG, "Logic: Target Temp Updated to %.1f C", *temp);
            break;
        }
        case EVENT_CMD_FAN_SPEED: {
            aircook_set_speed(*(fan_speed_t *)event_data);
            ESP_LOGI(TAG, "Logic: Fan Speed Updated to Enum: %d", *(fan_speed_t *)event_data);
            break;
        }
        case EVENT_TEMP_UPDATED: {

            break;
        }
        case EVENT_QR_CODE_READY: {
            // // 事件携带了可用于生成二维码的 URI 字符串 (wifi.c 传来的内容)
            WIFI_STATE = WIFI_STATE_PROVISIONING;
            ESP_LOGI(TAG, "Logic: QR Code Ready for Wi-Fi provisioning!");
            ui_wifi_up(WIFI_STATE);
            break;
        }
        case EVENT_WIFI_CONNECTED: {
            ESP_LOGI(TAG, "Logic: Wi-Fi connected successfully!");
            WIFI_STATE = WIFI_STATE_CONNECTED;
            ui_wifi_up(WIFI_STATE);
            break;
        }
        case EVENT_WIFI_DISCONNECTED:{
            WIFI_STATE = WIFI_STATE_DISCONNECTED;
            ui_wifi_up(WIFI_STATE);
            ESP_LOGI(TAG, "Logic: Wi-Fi disconnected!");
            break;
        }
        default:
            ESP_LOGW(TAG, "Logic: Unhandled event ID: %d", id);
            break;
    }
}


void app_event_init (void)
{
    esp_event_loop_args_t loop_args = {
        .queue_size = 10,
        .task_name = "my_event_task", 
        .task_priority = 5,
        .task_stack_size = 4096,
        .task_core_id = tskNO_AFFINITY
    };

    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &loop_handle));
    ESP_ERROR_CHECK(esp_event_handler_register_with(
    loop_handle, AIR_COOKER_EVENTS, ESP_EVENT_ANY_ID, app_event_handler, NULL));
}

