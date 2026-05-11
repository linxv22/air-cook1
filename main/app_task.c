#include "app_events.h"
#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
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
lv_timer_t * ui_timer = NULL;



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
            lv_label_set_text_fmt(Tem_label, "Cur Tem: %.1f °C", current_temp);
            uint32_t remain_s = aircook_gettime(); 
            if(remain_s > 0) {
                lv_label_set_text_fmt(Remain_time_label, "Remain: %02ld:%02ld", remain_s / 60, remain_s % 60);
         } 
            else {
                lv_label_set_text(Remain_time_label, "Remain: 00:00");
            }
            _lock_release(&lvgl_api_lock);
            break;
        }
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

    /* 顶部标题区 */
    lv_obj_t * title_label = lv_label_create(scr);
    lv_label_set_text(title_label, "AIR cook");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0); 
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 0); 

    /* 状态显示区 */
    Tem_label = lv_label_create(scr);
    lv_label_set_text(Tem_label, "Cur Tem: 25.0 °C");
    lv_obj_align(Tem_label, LV_ALIGN_TOP_LEFT, 10, 35); 

    Remain_time_label = lv_label_create(scr);
    lv_label_set_text(Remain_time_label, "Remain: 00:00");
    lv_obj_align(Remain_time_label, LV_ALIGN_TOP_RIGHT, -10, 35);

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

    _lock_release(&lvgl_api_lock);
}
