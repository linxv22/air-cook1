#include "app_events.h"
#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
#include "WIFI.h"

#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_task";
extern _lock_t lvgl_api_lock;
esp_event_loop_handle_t loop_handle;
ESP_EVENT_DEFINE_BASE(AIR_COOKER_EVENTS);

static char qr_url_buffer[256] = {0}; // 新增：保存配网数据的字符串缓存

// 工作状态，当前时间，当前温度，风扇转速，剩余时间，联网状态（顶层右上角WiFI小标志），语音识别的界面（识别到主人说话了就弹出，涉及到LVGL加字库）

WIFI_state_t WIFI_STATE = WIFI_STATE_INIT;

// ================= 事件中枢 (统一处理底层控制逻辑) =================
void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
    switch (id) {
        case EVENT_CMD_aircook: {
            // 接到开机指令，解包出温度和时间并下发给底层状态机
            // cook_config_t *cfg = (cook_config_t *)event_data;
            // aircook_set_speed(ui_fan_speed);
            // aircook_start(cfg);
            // ESP_LOGI(TAG, "Logic: Cook Started! Temp: %.1f C, Time: %ld s", cfg->temperature, cfg->time_s);
            break;
        }
        case EVENT_CMD_STOP: {
           
            break;
        }
        case EVENT_CMD_FAN_SPEED: {
            
            break;
        }
        case EVENT_TEMP_UPDATED: {
            break;
        }
        case EVENT_QR_CODE_READY: {
            // // 事件携带了可用于生成二维码的 URI 字符串 (wifi.c 传来的内容)
            // WIFI_STATE = WIFI_STATE_PROVISIONING;
            // //页面提示如何进行wifi配网

            // const char* qr_url = (const char*)event_data;
            // ESP_LOGI(TAG, "Logic: QR Code is ready, waiting for user to scan and connect... URL: %s", qr_url);
            
            // _lock_acquire(&lvgl_api_lock);
            //  // 【修改点】1: 缓存传过来的 URL 字符串
            // strncpy(qr_url_buffer, qr_url, sizeof(qr_url_buffer) - 1);
            
            // if (wifi_icon) {
            //     lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xCCCCCC), 0);
            // }
            
            // // 【修改点】2: 调用封装好的弹窗函数，替代它原本的绘制过程
            // show_qrcode_panel();
            // _lock_release(&lvgl_api_lock);
            break;
        }
        case EVENT_WIFI_CONNECTED: {
        //     ESP_LOGI(TAG, "Logic: Wi-Fi connected successfully!");
        //     WIFI_STATE = WIFI_STATE_CONNECTED;
        //     _lock_acquire(&lvgl_api_lock);
        //     // Wi-Fi 成功连接后触发销毁二维码
        // // 【修改点】3: 改为销毁整个弹窗面板
        //     if (qr_panel != NULL) {
        //         lv_obj_delete(qr_panel);
        //         qr_panel = NULL;
        //         ui_qrcode = NULL; 
        //         ESP_LOGI(TAG, "QR code panel removed from screen");
        //     }
            
        //     lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x006aff), 0);
        //     _lock_release(&lvgl_api_lock);
            break;
        }
        case EVENT_WIFI_DISCONNECTED:{
            //自动连接失败，底层自动启动配网模式
            // _lock_acquire(&lvgl_api_lock);
            // // 断开连接时，变为红色或灰色
            // if (wifi_icon) {
            //     //wifi断开连接，显示红色 #ff0000
            //     lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xFF0000), 0);
            // }
            // _lock_release(&lvgl_api_lock);
            
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

