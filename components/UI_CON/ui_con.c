#include "ui_con.h"
#include "app_events.h"
#include "lvgl.h"
#include "esp_log.h"

extern _lock_t lvgl_api_lock;

static const char *TAG = "UI_CON";

// ============ UI 局部状态缓存 ============ 
static int ui_target_temp = 180;
static int ui_target_time_min = 20;
static int ui_fan_speed = 80;
static wind_state_t current_wind_state = wind_main; // 当前风扇状态，默认主页面

/// ============ 界面与控件句柄 ============ 
lv_obj_t * scr_main;
lv_obj_t * scr_detail;
lv_obj_t * time_label; // 顶层状态栏的当前时间

// (保持你原有的这些句柄不变)
lv_obj_t * Tem_label;
lv_obj_t * Remain_time_label;
lv_obj_t * label_set_temp;
lv_obj_t * label_set_time;
lv_obj_t * label_set_fan;
lv_obj_t * ui_qrcode = NULL;  
lv_obj_t * wifi_icon = NULL;
lv_obj_t * qr_panel = NULL;   


// 关闭“X”按钮被点击的回调
static void close_qr_btn_cb(lv_event_t * e)
{
    if (qr_panel != NULL) {
        lv_obj_delete(qr_panel);
        qr_panel = NULL;
        ui_qrcode = NULL; // 面板被删，内部的二维码也一并被销毁了
    }
}

// 主界面食物模式点击事件
static void preset_btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const char * food_type = (const char *)lv_event_get_user_data(e); 
    
    if(code == LV_EVENT_CLICKED) {
        if (strcmp(food_type, "Fries") == 0) {
            ui_target_temp = 180;
            ui_target_time_min = 15;
            ui_fan_speed = 80;
        } else if (strcmp(food_type, "Chicken") == 0) {
            ui_target_temp = 200;
            ui_target_time_min = 25;
            ui_fan_speed = 100;
        } else if (strcmp(food_type, "Steak") == 0) {
            ui_target_temp = 200;
            ui_target_time_min = 12;
            ui_fan_speed = 100;
        }
        
        // 更新详细界面的 Label 值 (这部分控件如果在 ui_start 中已经被创建了，可以直接更新)
        lv_label_set_text_fmt(label_set_temp, "%d °C", ui_target_temp);
        lv_label_set_text_fmt(label_set_time, "%d min", ui_target_time_min);
        lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", ui_fan_speed);
        
        // 切换到详细控制界面
        lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    }
}

// “返回主页”按钮回调
static void back_btn_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    }
}

// 封装一个在屏幕中央弹出二维码界面的函数
static void show_qrcode_panel(void)
{
    // // 如果没有配网数据，直接返回
    // if (strlen(qr_url_buffer) == 0) return; 

    // // 如果面板已经存在，先清理以防重叠
    // if (qr_panel != NULL) {
    //     lv_obj_delete(qr_panel);
    //     qr_panel = NULL;
    // }

    // lv_obj_t * scr = lv_screen_active();
    
    // // 1. 创建背景容器面板
    // qr_panel = lv_obj_create(scr);
    // lv_obj_set_size(qr_panel, 200, 200); // 调整面板大小适应文字和按键
    // lv_obj_center(qr_panel);
    // lv_obj_set_style_bg_color(qr_panel, lv_color_hex(0xFFFFFF), 0);
    
    // lv_obj_clear_flag(qr_panel, LV_OBJ_FLAG_SCROLLABLE); 
    // // 2. 在右上角创建带有“X”符号的关闭按钮
    // lv_obj_t * btn_close = lv_button_create(qr_panel);
    // lv_obj_set_size(btn_close, 35, 35);
    // lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, 10, -10);
    // lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFF0000), 0); // 红色关闭建
    // lv_obj_add_event_cb(btn_close, close_qr_btn_cb, LV_EVENT_CLICKED, NULL);
    
    // lv_obj_t * label_close = lv_label_create(btn_close);
    // lv_label_set_text(label_close, LV_SYMBOL_CLOSE);
    // lv_obj_center(label_close);

    // // 3. 提示文字（可选）
    // lv_obj_t * title = lv_label_create(qr_panel);
    // lv_label_set_text(title, "Scan to Connect");
    // lv_obj_align(title, LV_ALIGN_TOP_LEFT, -5, 0);

    // // 4. 创建二维码
    // ui_qrcode = lv_qrcode_create(qr_panel); 
    // lv_obj_set_size(ui_qrcode, 130, 130);
    // lv_qrcode_update(ui_qrcode, qr_url_buffer, strlen(qr_url_buffer));
    // lv_obj_align(ui_qrcode, LV_ALIGN_BOTTOM_MID, 0, 10);
}

// 顶部 Wi-Fi 图标被点击的回调
static void wifi_icon_click_cb(lv_event_t * e)
{
    // 若当前未连接 Wi-Fi，且配网链接不为空，则再次呼出面板
    // if (WIFI_STATE != WIFI_STATE_CONNECTED) {
    //     show_qrcode_panel();
    // }
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
    
    // =========== 1. 创建两个独立的 Screen 对象 ===========
    scr_main = lv_obj_create(NULL);
    scr_detail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(scr_detail, lv_color_hex(0xFFFFFF), 0);
    
    // =========== 2. 设置顶层状态栏 (全局悬浮) ===========
    lv_obj_t * top_layer = lv_layer_top(); 
    
    // 左上角: 当前时间
    time_label = lv_label_create(top_layer);
    lv_label_set_text(time_label, "12:00"); 
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x000000), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 10, 10);
    
    // 右上角: WiFi 图标
    wifi_icon = lv_label_create(top_layer);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI); 
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x888888), 0); 
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_CLICKABLE); 
    lv_obj_set_ext_click_area(wifi_icon, 20); 
    lv_obj_add_event_cb(wifi_icon, wifi_icon_click_cb, LV_EVENT_CLICKED, NULL);

    // =========== 3. 绘制主界面 (scr_main) ===========
    lv_obj_t * title = lv_label_create(scr_main);
    lv_label_set_text(title, "Select Food");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * btn_fries = lv_button_create(scr_main);
    lv_obj_set_size(btn_fries, 100, 40);
    lv_obj_align(btn_fries, LV_ALIGN_CENTER, 0, -50);
    lv_obj_add_event_cb(btn_fries, preset_btn_event_cb, LV_EVENT_CLICKED, "Fries");
    lv_obj_t * label_fries = lv_label_create(btn_fries);
    lv_label_set_text(label_fries, "Fries");
    lv_obj_center(label_fries);

    lv_obj_t * btn_chicken = lv_button_create(scr_main);
    lv_obj_set_size(btn_chicken, 100, 40);
    lv_obj_align(btn_chicken, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_chicken, preset_btn_event_cb, LV_EVENT_CLICKED, "Chicken");
    lv_obj_t * label_chicken = lv_label_create(btn_chicken);
    lv_label_set_text(label_chicken, "Chicken");
    lv_obj_center(label_chicken);

    lv_obj_t * btn_steak = lv_button_create(scr_main);
    lv_obj_set_size(btn_steak, 100, 40);
    lv_obj_align(btn_steak, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_event_cb(btn_steak, preset_btn_event_cb, LV_EVENT_CLICKED, "Steak");
    lv_obj_t * label_steak = lv_label_create(btn_steak);
    lv_label_set_text(label_steak, "Steak");
    lv_obj_center(label_steak);

    // =========== 4. 绘制详细界面 (scr_detail) ===========
    // 状态显示区
    Tem_label = lv_label_create(scr_detail);
    lv_label_set_recolor(Tem_label, true); 
    lv_label_set_text(Tem_label, "Cur Tem: #006aff 25.0 °C#");
    lv_obj_align(Tem_label, LV_ALIGN_TOP_LEFT, 10, 45); 

    Remain_time_label = lv_label_create(scr_detail);
    lv_label_set_text(Remain_time_label, "Rem: 00:00");
    lv_obj_align(Remain_time_label, LV_ALIGN_TOP_RIGHT, -10, 45);

    /* 参数调控区 */
    create_ui_btn(scr_detail, "-", -70, -50, "TEMP-");
    label_set_temp = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_temp, "%d °C", ui_target_temp);
    lv_obj_align(label_set_temp, LV_ALIGN_CENTER, 0, -50);
    create_ui_btn(scr_detail, "+", 70, -50, "TEMP+");

    create_ui_btn(scr_detail, "-", -70, 0, "TIME-");
    label_set_time = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_time, "%d min", ui_target_time_min);
    lv_obj_align(label_set_time, LV_ALIGN_CENTER, 0, 0);
    create_ui_btn(scr_detail, "+", 70, 0, "TIME+");

    create_ui_btn(scr_detail, "-", -70, 50, "FAN-");
    label_set_fan = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", ui_fan_speed);
    lv_obj_align(label_set_fan, LV_ALIGN_CENTER, 0, 50);
    create_ui_btn(scr_detail, "+", 70, 50, "FAN+");

    /* 底部操作区 */
    lv_obj_t* btn_start = create_ui_btn(scr_detail, "START", -50, 100, "START");
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x187600), 0); 
    lv_obj_t* btn_stop = create_ui_btn(scr_detail, "STOP", 50, 100, "STOP");
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0xFF0000), 0); 

    // 添加一个“返回”按钮 (放到左下角)
    lv_obj_t* btn_back = lv_button_create(scr_detail);
    lv_obj_set_size(btn_back, 60, 40);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_add_event_cb(btn_back, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Back");
    lv_obj_center(label_back);

    // =========== 5. 载入默认界面 ===========
    lv_scr_load(scr_main);

    _lock_release(&lvgl_api_lock);
}
