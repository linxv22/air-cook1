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
#include "my_audio.h"

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
            cook_state_t current_state = aircook_getstate(); // 先获取状态，看看能不能开始烹饪
            if (current_state == cook_stopped) {
                cook_config_t *cfg = (cook_config_t *)event_data;
                aircook_start(cfg);
                ESP_LOGI(TAG, "Logic: Cook Started! Temp: %.1f C, Time: %ld s, Fan Enum: %d", 
                         cfg->temperature, cfg->time_s, cfg->fan_speed);
            } else {
                ESP_LOGW(TAG, "Logic: Cannot start cooking, current state is not stopped!");
            }
            break;
        }
        case EVENT_CMD_STOP: {
            aircook_stop();
            ESP_LOGI(TAG, "Logic: Cook Stopped!");
            break;
        }
        case EVENT_CMD_FINISH: {
            ESP_LOGI(TAG, "Logic: Cook Finished! ");
            ui_show_cooking_complete();
            //预留声音播报接口，等音频模块做好了再来调用
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
            // if(aircook_getstate() == cook_running)
            ui_up_temp(ntc_adc_read_temperature(), aircook_gettime());
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
            dpp_deinit();
            ui_wifi_up(WIFI_STATE);
            time_sntp_init();
            websocket_clint_init();
            break;
        }
        case EVENT_WIFI_DISCONNECTED:{
            WIFI_STATE = WIFI_STATE_DISCONNECTED;
            dpp_enrollee_bootstrap(); // 连接断开时自动重启 DPP 配网流程
            ui_wifi_up(WIFI_STATE);
            ESP_LOGI(TAG, "Logic: Wi-Fi disconnected!");
            break;
        }
        case EVENT_AUDIO_CMD: {
            ui_mic_state_update( *(mic_state_t *)event_data);
            break;
        }
        case EVENT_CLOUD_DATA: {
            cloud_data_t *cloud_data = (cloud_data_t *)event_data;    
            if(aircook_getstate() == cook_stopped) { // 只有在空闲状态才接受云端命令开始烹饪
                ui_show_cloud_detail(cloud_data); // 先展示云端数据到界面上
                ESP_LOGI(TAG, "Logic: Cloud command executed! Temp: %.1f C, Time: %ld s, Fan Enum: %d, Food: %s", 
                         cloud_data->temperature, cloud_data->time_s, cloud_data->fan_speed, cloud_data->food_name);
            } else {
                ESP_LOGW(TAG, "Logic: Cannot execute cloud command, current state is not stopped!");
            }
            break;
        }
        case EVENT_CLOUD_CMD:{
            cloud_cmd_t *cloud_cmd = (cloud_cmd_t *)event_data;
            if(*cloud_cmd == cloud_cmd_start) {
                if(aircook_getstate() == cook_stopped) { // 只有在空闲状态才接受云端命令开始烹饪
                    ui_cloud_start(); // 直接触发开始烹饪的事件（等同于按下 START 按钮）
                    ESP_LOGI(TAG, "Logic: Cloud command to START cooking executed!");
                } else {
                    ESP_LOGW(TAG, "Logic: Cannot execute cloud START command, current state is not stopped!");
                }
            } else if (*cloud_cmd == cloud_cmd_stop) {
                aircook_stop();
                ESP_LOGI(TAG, "Logic: Cloud command to STOP cooking executed!");
            }
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
        .task_stack_size = 4096 ,
        .task_core_id = tskNO_AFFINITY,
    };

    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &loop_handle));
    ESP_ERROR_CHECK(esp_event_handler_register_with(
    loop_handle, AIR_COOKER_EVENTS, ESP_EVENT_ANY_ID, app_event_handler, NULL));
}
