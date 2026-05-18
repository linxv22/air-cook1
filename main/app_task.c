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

// ============ UI 局部状态缓存 ============ 
static int ui_target_temp = 180;
static int ui_target_time_min = 20;
static int ui_fan_speed = 80;

// ============ UI 控件句柄 ============ 
lv_obj_t * Tem_label;
lv_obj_t * Remain_time_label;
lv_obj_t * label_set_temp;
lv_obj_t * label_set_time;
lv_obj_t * label_set_fan;
lv_obj_t * ui_qrcode = NULL;  // 新增：Wi-Fi 连接二维码句柄
lv_obj_t * wifi_icon = NULL;
lv_obj_t * qr_panel = NULL;   // 新增：包含二维码和关闭按钮的弹窗面板
static char qr_url_buffer[256] = {0}; // 新增：保存配网数据的字符串缓存

// 工作状态，当前时间，当前温度，风扇转速，剩余时间，联网状态（顶层右上角WiFI小标志），语音识别的界面（识别到主人说话了就弹出，涉及到LVGL加字库）

static void show_qrcode_panel(void);



WIFI_state_t WIFI_STATE = WIFI_STATE_INIT;

// ================= 事件中枢 (统一处理底层控制逻辑) =================
void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
    switch (id) {
        case EVENT_CMD_aircook: {
            // 接到开机指令，解包出温度和时间并下发给底层状态机
            cook_config_t *cfg = (cook_config_t *)event_data;
            aircook_set_speed(ui_fan_speed);
            aircook_start(cfg);
            ESP_LOGI(TAG, "Logic: Cook Started! Temp: %.1f C, Time: %ld s", cfg->temperature, cfg->time_s);
            break;
        }
        case EVENT_CMD_STOP: {
            aircook_stop();
            ESP_LOGI(TAG, "Logic: Cook Stopped!");
            break;
        }
        case EVENT_CMD_FAN_SPEED: {
            uint32_t speed = *(uint32_t *)event_data;
            aircook_set_speed(speed);
            ESP_LOGI(TAG, "Logic: Fan speed updated to %ld%%", speed);
            break;
        }
        case EVENT_TEMP_UPDATED: {
            _lock_acquire(&lvgl_api_lock);
            float current_temp = ntc_adc_read_temperature();
            if(current_temp <100)
            lv_label_set_text_fmt(Tem_label, "Cur Tem: #2149e9 %.1f # °C", current_temp);
            else
            lv_label_set_text_fmt(Tem_label, "Cur Tem: #ff0000 %.1f # °C", current_temp);
            uint32_t remain_s = aircook_gettime(); 
            if(remain_s > 0) {
                lv_label_set_text_fmt(Remain_time_label, "Rem %02ld:%02ld", remain_s / 60, remain_s % 60);
            } 
            else {
                lv_label_set_text(Remain_time_label, "Rem:00:00");
            }
            _lock_release(&lvgl_api_lock);
            break;
        }
        case EVENT_QR_CODE_READY: {
            // 事件携带了可用于生成二维码的 URI 字符串 (wifi.c 传来的内容)
            if(WIFI_STATE == WIFI_STATE_DISCONNECTED)
            {
                //wifi连接失败重新启动dpp配网功能
                //配套设计ui界面
            }
            else if(WIFI_STATE == WIFI_STATE_INIT )
            {
                //首次启动，flash无wifi信息
                //配套设计ui界面
            }
            WIFI_STATE = WIFI_STATE_PROVISIONING;
            //页面提示如何进行wifi配网

            const char* qr_url = (const char*)event_data;
            ESP_LOGI(TAG, "Logic: QR Code is ready, waiting for user to scan and connect... URL: %s", qr_url);
            
            _lock_acquire(&lvgl_api_lock);
             // 【修改点】1: 缓存传过来的 URL 字符串
            strncpy(qr_url_buffer, qr_url, sizeof(qr_url_buffer) - 1);
            
            if (wifi_icon) {
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xCCCCCC), 0);
            }
            
            // 【修改点】2: 调用封装好的弹窗函数，替代它原本的绘制过程
            show_qrcode_panel();
            _lock_release(&lvgl_api_lock);
            break;
        }
        case EVENT_WIFI_CONNECTED: {
            ESP_LOGI(TAG, "Logic: Wi-Fi connected successfully!");
            WIFI_STATE = WIFI_STATE_CONNECTED;
            _lock_acquire(&lvgl_api_lock);
            // Wi-Fi 成功连接后触发销毁二维码
        // 【修改点】3: 改为销毁整个弹窗面板
            if (qr_panel != NULL) {
                lv_obj_delete(qr_panel);
                qr_panel = NULL;
                ui_qrcode = NULL; 
                ESP_LOGI(TAG, "QR code panel removed from screen");
            }
            
            lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x006aff), 0);
            _lock_release(&lvgl_api_lock);
            break;
        }
        case EVENT_WIFI_DISCONNECTED:{
            WIFI_STATE = WIFI_STATE_DISCONNECTED;
            //自动连接失败，底层自动启动配网模式
            _lock_acquire(&lvgl_api_lock);
            // 断开连接时，变为红色或灰色
            if (wifi_icon) {
                //wifi断开连接，显示红色 #ff0000
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xFF0000), 0);
            }
            _lock_release(&lvgl_api_lock);
            
            break;
        }
        default:
            ESP_LOGW(TAG, "Logic: Unhandled event ID: %d", id);
            break;
    }
}

// 关闭“X”按钮被点击的回调
static void close_qr_btn_cb(lv_event_t * e)
{
    if (qr_panel != NULL) {
        lv_obj_delete(qr_panel);
        qr_panel = NULL;
        ui_qrcode = NULL; // 面板被删，内部的二维码也一并被销毁了
    }
}

// 封装一个在屏幕中央弹出二维码界面的函数
static void show_qrcode_panel(void)
{
    // 如果没有配网数据，直接返回
    if (strlen(qr_url_buffer) == 0) return; 

    // 如果面板已经存在，先清理以防重叠
    if (qr_panel != NULL) {
        lv_obj_delete(qr_panel);
        qr_panel = NULL;
    }

    lv_obj_t * scr = lv_screen_active();
    
    // 1. 创建背景容器面板
    qr_panel = lv_obj_create(scr);
    lv_obj_set_size(qr_panel, 200, 200); // 调整面板大小适应文字和按键
    lv_obj_center(qr_panel);
    lv_obj_set_style_bg_color(qr_panel, lv_color_hex(0xFFFFFF), 0);
    
    lv_obj_clear_flag(qr_panel, LV_OBJ_FLAG_SCROLLABLE); 
    // 2. 在右上角创建带有“X”符号的关闭按钮
    lv_obj_t * btn_close = lv_button_create(qr_panel);
    lv_obj_set_size(btn_close, 35, 35);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, 10, -10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFF0000), 0); // 红色关闭建
    lv_obj_add_event_cb(btn_close, close_qr_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t * label_close = lv_label_create(btn_close);
    lv_label_set_text(label_close, LV_SYMBOL_CLOSE);
    lv_obj_center(label_close);

    // 3. 提示文字（可选）
    lv_obj_t * title = lv_label_create(qr_panel);
    lv_label_set_text(title, "Scan to Connect");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, -5, 0);

    // 4. 创建二维码
    ui_qrcode = lv_qrcode_create(qr_panel); 
    lv_obj_set_size(ui_qrcode, 130, 130);
    lv_qrcode_update(ui_qrcode, qr_url_buffer, strlen(qr_url_buffer));
    lv_obj_align(ui_qrcode, LV_ALIGN_BOTTOM_MID, 0, 10);
}

// 顶部 Wi-Fi 图标被点击的回调
static void wifi_icon_click_cb(lv_event_t * e)
{
    // 若当前未连接 Wi-Fi，且配网链接不为空，则再次呼出面板
    if (WIFI_STATE != WIFI_STATE_CONNECTED) {
        show_qrcode_panel();
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

// ================= UI 与 按键回调 =================

// 按键点击回调: 纯页面计算 + 投递事件。彻底和硬件解耦！
static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const char * btn_id = (const char *)lv_event_get_user_data(e); 
    
    if(code == LV_EVENT_CLICKED) { // 建议用 CLICKED，体验更好
        
        if (strcmp(btn_id, "TEMP-") == 0) {
            if(ui_target_temp > 40) ui_target_temp -= 5;
            lv_label_set_text_fmt(label_set_temp, "%d °C", ui_target_temp);

        } else if (strcmp(btn_id, "TEMP+") == 0) {
            if(ui_target_temp < 220) ui_target_temp += 5;
            lv_label_set_text_fmt(label_set_temp, "%d °C", ui_target_temp);

        } else if (strcmp(btn_id, "TIME-") == 0) {
            if(ui_target_time_min > 1) ui_target_time_min -= 1;
            lv_label_set_text_fmt(label_set_time, "%d min", ui_target_time_min);

        } else if (strcmp(btn_id, "TIME+") == 0) {
            if(ui_target_time_min < 60) ui_target_time_min += 1;
            lv_label_set_text_fmt(label_set_time, "%d min", ui_target_time_min);

        } else if (strcmp(btn_id, "FAN-") == 0) {
            if(ui_fan_speed > 20) ui_fan_speed -= 10;
            lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", ui_fan_speed);
            // 实时调风扇，投递消息给控制层
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN_SPEED, &ui_fan_speed, sizeof(ui_fan_speed), 0);

        } else if (strcmp(btn_id, "FAN+") == 0) {
            if(ui_fan_speed < 100) ui_fan_speed += 10;
            lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", ui_fan_speed);
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN_SPEED, &ui_fan_speed, sizeof(ui_fan_speed), 0);

        } else if (strcmp(btn_id, "START") == 0) {
            // 打包数据，发给中枢执行
            cook_config_t cfg = {
                .temperature = (float)ui_target_temp,
                .time_s = (uint32_t)(ui_target_time_min * 60)
            };
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_aircook, &cfg, sizeof(cfg), 0);

        } else if (strcmp(btn_id, "STOP") == 0) {
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_STOP, NULL, 0, 0);
        }
    }
}

// 辅助创建按钮包装器
static lv_obj_t * create_ui_btn(lv_obj_t * parent, const char * txt, int x, int y, const char * btn_id)
{
    lv_obj_t * btn = lv_button_create(parent); // 注意：LVGL v8 用 lv_btn_create 
    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_center(label);
    
    // 设置位置和点击事件
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)btn_id);
    return btn;
}

void ui_start(void)
{
    _lock_acquire(&lvgl_api_lock);
    lv_obj_t * scr = lv_screen_active();
    // 设置屏幕背景色 #FFFFFF
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);

    wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI); // 使用内置WiFi符号
    // 初始化为灰色表示未连接 #888888
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x888888), 0); 
    // 对齐到右上角，偏移量 x=-10, y=10 
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -10, 10);
    /* ----------------------------- */

    /* 状态显示区 */
    Tem_label = lv_label_create(scr);
    // 1. 开启该标签的重新着色支持
    lv_label_set_recolor(Tem_label, true); 
    //  #22a0a4 上，VS Code 会自动弹调色板
    lv_label_set_text(Tem_label, "Cur Tem: #006aff 25.0 °C#");
    lv_obj_align(Tem_label, LV_ALIGN_TOP_LEFT, 10, 45); 

    Remain_time_label = lv_label_create(scr);
    lv_label_set_text(Remain_time_label, "Rem: 00:00");
    lv_obj_align(Remain_time_label, LV_ALIGN_TOP_RIGHT, -10, 45);

    /* 参数调控区 (y 轴通过像素偏移) */
    
    // 1. 设置温度
    create_ui_btn(scr, "-", -70, -50, "TEMP-");
    label_set_temp = lv_label_create(scr);
    lv_label_set_text_fmt(label_set_temp, "%d °C", ui_target_temp);
    lv_obj_align(label_set_temp, LV_ALIGN_CENTER, 0, -50);
    create_ui_btn(scr, "+", 70, -50, "TEMP+");

    // 2. 设置时间
    create_ui_btn(scr, "-", -70, 0, "TIME-");
    label_set_time = lv_label_create(scr);
    lv_label_set_text_fmt(label_set_time, "%d min", ui_target_time_min);
    lv_obj_align(label_set_time, LV_ALIGN_CENTER, 0, 0);
    create_ui_btn(scr, "+", 70, 0, "TIME+");

    // 3. 设置风扇
    create_ui_btn(scr, "-", -70, 50, "FAN-");
    label_set_fan = lv_label_create(scr);
    lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", ui_fan_speed);
    lv_obj_align(label_set_fan, LV_ALIGN_CENTER, 0, 50);
    create_ui_btn(scr, "+", 70, 50, "FAN+");

    /* 底部操作区 */
    lv_obj_t* btn_start = create_ui_btn(scr, "START", -50, 100, "START");

    //(绿色: #187600)
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x187600), 0); 

    lv_obj_t* btn_stop = create_ui_btn(scr, "STOP", 50, 100, "STOP");
    //(红色: #ff0000)
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0xFF0000), 0); 

    wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI); 
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x888888), 0); 
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -10, 10);
    
    // ========【新增：添加交互属性】========
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_CLICKABLE); // 支持触控点击
    lv_obj_set_ext_click_area(wifi_icon, 20); // (可选) 增加图标周边的点击判定面积，防止指头太粗按不准
    lv_obj_add_event_cb(wifi_icon, wifi_icon_click_cb, LV_EVENT_CLICKED, NULL);
    // ===================================

    _lock_release(&lvgl_api_lock);
}
