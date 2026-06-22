#include "ui_con.h"

#include "lvgl.h"
#include "esp_log.h"
#include <time.h> 


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern _lock_t lvgl_api_lock;
extern WIFI_state_t WIFI_STATE;

LV_FONT_DECLARE(my_font);
LV_FONT_DECLARE(kaiTI);

static const char *TAG = "UI_CON";

// ============ UI 局部状态缓存 ============ 
static cook_config_t current_config = {
    .temperature = 180.0f,
    .time_s = 15 * 60,
    .fan_speed = fan_mid,
    .food_name = "",
};

static wind_state_t current_wind_state = wind_main; // 当前窗口状态，默认主页面
static const char * qr_url_buffer = NULL; // 替换原来的 char 数组

typedef enum {
    // 预设模式按钮
    BTN_ID_PRESET_FRIES,//薯条模式
    BTN_ID_PRESET_CHICKEN,//炸鸡模式
    BTN_ID_PRESET_STEAK,//牛排模式
    // 详细控制界面按钮
    BTN_ID_TEMP_MINUS,// 温度减
    BTN_ID_TEMP_PLUS,// 温度加
    BTN_ID_TIME_MINUS,// 时间减
    BTN_ID_TIME_PLUS,// 时间加
    BTN_ID_FAN_MINUS,// 风速减
    BTN_ID_FAN_PLUS,// 风速加
    BTN_ID_START// 开始烹饪
} btn_id_t;

/// ============ 界面与控件句柄 ============ 
static lv_obj_t * scr_main = NULL;   // 主界面
static lv_obj_t * scr_detail = NULL; // 详细控制界面
static lv_obj_t * scr_complete = NULL; // 烹饪完成界面
static lv_obj_t * scr_cooking = NULL; // 烹饪中界面

// 顶层状态栏控件句柄
static lv_obj_t * time_label = NULL; // 顶层状态栏的当前时间
static lv_obj_t * wifi_icon = NULL;
static lv_obj_t * mic_icon = NULL;

// 详细界面句柄
static lv_obj_t * Tem_label = NULL; // 详细界面显示当前温度的标签
static lv_obj_t * label_set_food = NULL;
static lv_obj_t * label_set_temp = NULL;
static lv_obj_t * label_set_time = NULL;
static lv_obj_t * label_set_fan = NULL;

// 二维码界面句柄
static lv_obj_t * ui_qrcode = NULL;  
static lv_obj_t * qr_panel = NULL;   


// ============ 动态按钮句柄 ============
static lv_obj_t * btn_back = NULL;
static lv_obj_t * btn_start = NULL;

// 加减调节按钮句柄
static lv_obj_t * btn_temp_minus = NULL;
static lv_obj_t * btn_temp_plus  = NULL;
static lv_obj_t * btn_time_minus = NULL;
static lv_obj_t * btn_time_plus  = NULL;
static lv_obj_t * btn_fan_minus  = NULL;
static lv_obj_t * btn_fan_plus   = NULL;

// ============ 烹饪中界面 (scr_cooking) 控件句柄 ============
static lv_obj_t * cook_food_label = NULL;   // 食物名称
static lv_obj_t * cook_temp_label = NULL;   // 实时温度（大字体）
static lv_obj_t * cook_time_label = NULL;   // 剩余时间倒计时
static lv_obj_t * cook_fan_label  = NULL;   // 当前风速
static lv_obj_t * btn_stop_cook   = NULL;   // 烹饪界面的停止按钮


// ============ LVGL 事件回调函数声明 ============
// ================= UI 与 按键回调 =================
static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    btn_id_t btn_id = (btn_id_t)(uintptr_t)lv_event_get_user_data(e); 
    
    if(code == LV_EVENT_CLICKED) { 
        switch (btn_id) {
            case BTN_ID_TEMP_MINUS:
                if(current_config.temperature > 40.0f) current_config.temperature -= 5.0f;
                lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
                break;
            case BTN_ID_TEMP_PLUS:
                if(current_config.temperature < 220.0f) current_config.temperature += 5.0f;
                lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
                break;
            case BTN_ID_TIME_MINUS:
                if(current_config.time_s > 60) current_config.time_s -= 60;
                lv_label_set_text_fmt(label_set_time, "%d min", (int)current_config.time_s / 60);
                break;
            case BTN_ID_TIME_PLUS:
                if(current_config.time_s < 60 * 60) current_config.time_s += 60;
                lv_label_set_text_fmt(label_set_time, "%d min", (int)current_config.time_s / 60);
                break;
             case BTN_ID_FAN_MINUS:
                // 由于定义是 fan_high(0) -> fan_mid(1) -> fan_low(2) ；风速减需要增加枚举的值
                if (current_config.fan_speed < fan_low) {
                    current_config.fan_speed++;
                }
                    switch (current_config.fan_speed) {
                        case fan_high:
                            lv_label_set_text_fmt(label_set_fan, "Fan: High");
                            break;
                        case fan_mid:
                            lv_label_set_text_fmt(label_set_fan, "Fan: Mid");
                            break;
                        case fan_low:
                            lv_label_set_text_fmt(label_set_fan, "Fan: Low");
                            break;
                        default:
                            lv_label_set_text_fmt(label_set_fan, "Fan: Unknown");
                            break;
                    }
                break;
            case BTN_ID_FAN_PLUS:
                // 风速加需要减小枚举的值
                if (current_config.fan_speed > fan_high) {
                    current_config.fan_speed--;
                }
                switch (current_config.fan_speed) {
                    case fan_high:
                        lv_label_set_text_fmt(label_set_fan, "Fan: High");
                        break;
                    case fan_mid:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Mid");
                        break;
                    case fan_low:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Low");
                        break;
                    default:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Unknown");
                        break;
                }
                break;
            case BTN_ID_START:
                {
                    cook_config_t cfg = {
                        .temperature = current_config.temperature,
                        .time_s = current_config.time_s,
                        .fan_speed = current_config.fan_speed,
                        .food_name = "",
                    };
                    snprintf(cfg.food_name, sizeof(cfg.food_name), "%s", current_config.food_name);
                    esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_aircook,
                                    &cfg, sizeof(cfg), 0);

                    // ✅ 同步信息到烹饪界面
                    if (cook_food_label != NULL)
                        lv_label_set_text(cook_food_label, lv_label_get_text(label_set_food));
                    if (cook_temp_label != NULL)
                        lv_label_set_text_fmt(cook_temp_label, "#006aff %.1f °C#", current_config.temperature);
                    if (cook_time_label != NULL)
                        lv_label_set_text_fmt(cook_time_label, "剩余 %d:%02d",
                            (int)current_config.time_s / 60, (int)current_config.time_s % 60);
                    if (cook_fan_label != NULL) {
                        const char *fan_str =
                            current_config.fan_speed == fan_high ? "High" :
                            current_config.fan_speed == fan_mid  ? "Mid"  : "Low";
                        lv_label_set_text_fmt(cook_fan_label, "风速  %s", fan_str);
                    }

                    // ✅ 切换到烹饪界面
                    lv_scr_load(scr_cooking);
                }
                break;

            default:
                break;
        }
    }
}
// 主界面食物模式点击事件
static void preset_btn_event_cb(lv_event_t * e)
{
    if (lv_screen_active() != scr_main) return;
    lv_event_code_t code = lv_event_get_code(e);
    // 从 user_data 中恢复枚举值，使用 uintptr_t 防止 64位系统强转警告
    btn_id_t btn_id = (btn_id_t)(uintptr_t)lv_event_get_user_data(e); 
    
    if(code == LV_EVENT_CLICKED) {
        if (btn_id == BTN_ID_PRESET_FRIES) {
            current_config.temperature = 180.0f;
            current_config.time_s = 15 * 60;
            current_config.fan_speed = fan_low;
            snprintf(current_config.food_name, sizeof(current_config.food_name), "薯条");
            lv_label_set_text(label_set_food, "薯条");
        } else if (btn_id == BTN_ID_PRESET_CHICKEN) {
            current_config.temperature = 200.0f;
            current_config.time_s = 25 * 60;
            current_config.fan_speed = fan_high;
            snprintf(current_config.food_name, sizeof(current_config.food_name), "炸鸡");
            lv_label_set_text(label_set_food, "炸鸡");
        } else if (btn_id == BTN_ID_PRESET_STEAK) {
            current_config.temperature = 200.0f;
            current_config.time_s = 12 * 60;
            current_config.fan_speed = fan_high;
            snprintf(current_config.food_name, sizeof(current_config.food_name), "牛排");
            lv_label_set_text(label_set_food, "牛排");
        }
        
        // 更新详细界面的 Label 值
        lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
        lv_label_set_text_fmt(label_set_time, "%d min", (int)current_config.time_s / 60);
         switch (current_config.fan_speed) {
                    case fan_high:
                        lv_label_set_text_fmt(label_set_fan, "Fan: High");
                        break;
                    case fan_mid:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Mid");
                        break;
                    case fan_low:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Low");
                        break;
                    default:
                        lv_label_set_text_fmt(label_set_fan, "Fan: Unknown");
                        break;
                }
        
        
        // 切换到详细控制界面
        lv_screen_load(scr_detail);
    }
}

// “返回主页”按钮回调
static void back_btn_event_cb(lv_event_t * e)
{
    if (lv_screen_active() != scr_detail) return;
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_screen_load(scr_main);
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
    //如果面板已经存在，先清理以防重叠
    if (qr_panel != NULL) {
        lv_obj_delete(qr_panel);
        qr_panel = NULL;
    }
     
    qr_url_buffer = wifi_get_dpp_uri(); // 确保 URI 已经被 wifi 组件准备好了（如果还没准备好，这个函数会返回空字符串）
    lv_obj_t * scr = lv_screen_active();
    
    //  1. 创建背景容器面板
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

    // 3. 提示文字
    lv_obj_t * title = lv_label_create(qr_panel);
    lv_label_set_text(title, "扫描连接Wi-Fi");
    lv_obj_set_style_text_font(title, &kaiTI, 0);
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
    //若当前未连接 Wi-Fi，且配网链接不为空，则再次呼出面板
    if (WIFI_STATE != WIFI_STATE_CONNECTED) {
        show_qrcode_panel();
    }
}

// "烹饪完成"页面 → 返回主页按钮回调
static void complete_back_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load(scr_main);
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_STOP, NULL, 0, 0);
    }
}

// 辅助创建按钮包装器
static lv_obj_t * create_ui_btn(lv_obj_t * parent, const char * txt, int x, int y, btn_id_t btn_id)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_size(btn, 50, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF5F5F5), 0);        // 浅灰底
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE0E0E0), LV_STATE_PRESSED); // 按下时深一点
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xBDBDBD), 0);    // 灰色边框
    lv_obj_set_style_radius(btn, 8, 0);                               // 圆角

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);    // 黑色文字
    lv_obj_set_style_text_font(label, &kaiTI, 0);                     // 楷体
    lv_obj_center(label);
    
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)btn_id);
    return btn;
}



// 烹饪界面 "停止" 按钮回调
static void cook_stop_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_STOP, NULL, 0, 0);
        lv_scr_load(scr_detail);
    }
}

// ============ 时间刷新定时器回调 ============
static void time_update_timer_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    char buf[16];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_year >= (2026 - 1900)) {  // 年份 >= 2026 说明 SNTP 已同步
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    
    if (time_label != NULL) {
        lv_label_set_text(time_label, buf);
    }
}

// ============ UI 相关函数实现 ============
void ui_start(void)
{
    _lock_acquire(&lvgl_api_lock);
    
    // =========== 1. 创建三个独立的 Screen 对象 ===========
    scr_main = lv_obj_create(NULL);
    scr_detail = lv_obj_create(NULL);
    scr_complete = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(scr_detail, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(scr_complete, lv_color_hex(0xFFFFFF), 0);
    
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

    // 右上角: 麦克风图标
    mic_icon = lv_label_create(top_layer);
    lv_label_set_text(mic_icon, "\uf130"); 
    lv_obj_set_style_text_font(mic_icon, &my_font, 0);
    lv_obj_align_to(mic_icon, wifi_icon, LV_ALIGN_OUT_LEFT_MID, -10, 0); // 在 Wifi 图标左边 10 像素

    // =========== 3. 绘制主界面 (scr_main) ===========
    // 标题
    lv_obj_t * title = lv_label_create(scr_main);
    lv_label_set_text(title, "烹饪模式");
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(title, &kaiTI, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // ── 薯条按钮 ──
    lv_obj_t * btn_fries = lv_button_create(scr_main);
    lv_obj_set_size(btn_fries, 150, 55);
    lv_obj_align(btn_fries, LV_ALIGN_CENTER, 0, -65);
    lv_obj_set_style_bg_color(btn_fries, lv_color_hex(0xFFF9C4), 0);   // 淡黄色
    lv_obj_set_style_border_width(btn_fries, 2, 0);
    lv_obj_set_style_border_color(btn_fries, lv_color_hex(0xF9A825), 0); // 金色边框
    lv_obj_add_event_cb(btn_fries, preset_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)BTN_ID_PRESET_FRIES);
    lv_obj_t * label_fries = lv_label_create(btn_fries);
    lv_label_set_text(label_fries, "薯条");
    lv_obj_set_style_text_color(label_fries, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_fries, &kaiTI, 0);
    lv_obj_center(label_fries);

    // ── 炸鸡按钮 ──
    lv_obj_t * btn_chicken = lv_button_create(scr_main);
    lv_obj_set_size(btn_chicken, 150, 55);
    lv_obj_align(btn_chicken, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn_chicken, lv_color_hex(0xFFE0B2), 0);   // 淡橙色
    lv_obj_set_style_border_width(btn_chicken, 2, 0);
    lv_obj_set_style_border_color(btn_chicken, lv_color_hex(0xFB8C00), 0); // 橙色边框
    lv_obj_add_event_cb(btn_chicken, preset_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)BTN_ID_PRESET_CHICKEN);
    lv_obj_t * label_chicken = lv_label_create(btn_chicken);
    lv_label_set_text(label_chicken, "炸鸡");
    lv_obj_set_style_text_color(label_chicken, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_chicken, &kaiTI, 0);
    lv_obj_center(label_chicken);

    // ── 牛排按钮 ──
    lv_obj_t * btn_steak = lv_button_create(scr_main);
    lv_obj_set_size(btn_steak, 150, 55);
    lv_obj_align(btn_steak, LV_ALIGN_CENTER, 0, 65);
    lv_obj_set_style_bg_color(btn_steak, lv_color_hex(0xFFCCBC), 0);   // 淡珊瑚色
    lv_obj_set_style_border_width(btn_steak, 2, 0);
    lv_obj_set_style_border_color(btn_steak, lv_color_hex(0xD84315), 0); // 红棕边框
    lv_obj_add_event_cb(btn_steak, preset_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)BTN_ID_PRESET_STEAK);
    lv_obj_t * label_steak = lv_label_create(btn_steak);
    lv_label_set_text(label_steak, "牛排");
    lv_obj_set_style_text_color(label_steak, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_steak, &kaiTI, 0);
    lv_obj_center(label_steak);

    // =========== 4. 绘制详细界面 (scr_detail) ===========
    // 状态显示区
    Tem_label = lv_label_create(scr_detail);
    // 等到 LVGL 9 才可以用 lv_label_set_text_fmt 直接写带颜色的标签，使用旧版格式化注意
    // 或者直接使用文本拼接：
    // lv_label_set_recolor(Tem_label, true); // 此函数在 LVGL v8 被废除，转而使用 style 
    // 若原库是 v8 ，您这行可能编译失败或有效，这里保持您原带的样子。
    lv_label_set_recolor(Tem_label, true); 
    lv_label_set_text(Tem_label, "#006aff 25.0 °C#");
    lv_obj_set_style_text_font(Tem_label, &lv_font_montserrat_24 , 0);
    lv_obj_align(Tem_label, LV_ALIGN_TOP_LEFT, 10, 45); 

    label_set_food = lv_label_create(scr_detail);
    lv_label_set_text(label_set_food, "xxx");
    lv_obj_set_style_text_font(label_set_food, &kaiTI , 0);
    lv_obj_align(label_set_food, LV_ALIGN_TOP_RIGHT, -10, 45);

    /* 参数调控区：这里已经全面更改为枚举传递 */
    btn_temp_minus = create_ui_btn(scr_detail, "-", -70, -50, BTN_ID_TEMP_MINUS);
    
    label_set_temp = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
    lv_obj_align(label_set_temp, LV_ALIGN_CENTER, 0, -50);
    btn_temp_plus = create_ui_btn(scr_detail, "+", 70, -50, BTN_ID_TEMP_PLUS);

    btn_time_minus = create_ui_btn(scr_detail, "-", -70, 0, BTN_ID_TIME_MINUS);
    label_set_time = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_time, "%d min",(int) current_config.time_s / 60);
    lv_obj_align(label_set_time, LV_ALIGN_CENTER, 0, 0);
    btn_time_plus = create_ui_btn(scr_detail, "+", 70, 0, BTN_ID_TIME_PLUS);

    btn_fan_minus = create_ui_btn(scr_detail, "-", -70, 50, BTN_ID_FAN_MINUS);
    label_set_fan = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_fan, "Fan: Mid");
    lv_obj_align(label_set_fan, LV_ALIGN_CENTER, 0, 50);
    btn_fan_plus = create_ui_btn(scr_detail, "+", 70, 50, BTN_ID_FAN_PLUS);


   /* ──── 底部操作区（改为隐藏/显示模式）──── */
    // ✅ Back 按钮（始终先创建）
    btn_back = lv_button_create(scr_detail);
    lv_obj_set_size(btn_back, 90, 45);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, -50, -20);
    lv_obj_add_event_cb(btn_back, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "返回");
    lv_obj_set_style_text_color(label_back, lv_color_hex(0x000000), 0);    // 黑色文字
    lv_obj_set_style_text_font(label_back, &kaiTI, 0);
    lv_obj_center(label_back);

    // ✅ START 按钮（创建后可见）
    btn_start = lv_button_create(scr_detail);
    lv_obj_set_size(btn_start, 90, 45);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, +50, -20);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x187600), 0);
    lv_obj_add_event_cb(btn_start, btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)BTN_ID_START);
    lv_obj_t * label_start = lv_label_create(btn_start);
    lv_label_set_text(label_start, "开始");
    lv_obj_set_style_text_color(label_start, lv_color_hex(0x000000), 0);    // 黑色文字
    lv_obj_set_style_text_font(btn_start, &kaiTI, 0);
    lv_obj_center(label_start);

    // =========== 4.5. 烹饪中界面 (scr_cooking) ===========
    scr_cooking = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_cooking, lv_color_hex(0xFFFFFF), 0);

    // ── 食物名称 ──
    cook_food_label = lv_label_create(scr_cooking);
    lv_label_set_text(cook_food_label, "薯条");
    lv_obj_set_style_text_color(cook_food_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(cook_food_label, &kaiTI, 0);
    lv_obj_align(cook_food_label, LV_ALIGN_TOP_MID, 0, 35);

    // ── 实时温度（大字体居中）──
    cook_temp_label = lv_label_create(scr_cooking);
    lv_label_set_recolor(cook_temp_label, true);
    lv_label_set_text(cook_temp_label, "#006aff 25.0 °C#");
    lv_obj_set_style_text_font(cook_temp_label, &lv_font_montserrat_24, 0);
    lv_obj_align(cook_temp_label, LV_ALIGN_CENTER, 0, -30);

    // ── 剩余时间倒计时 ──
    cook_time_label = lv_label_create(scr_cooking);
    lv_label_set_text(cook_time_label, "剩余 15:00");
    lv_obj_set_style_text_color(cook_time_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(cook_time_label, &kaiTI, 0);
    lv_obj_align(cook_time_label, LV_ALIGN_CENTER, 0, 25);

    // ── 风速指示 ──
    cook_fan_label = lv_label_create(scr_cooking);
    lv_label_set_text(cook_fan_label, "风速  Mid");
    lv_obj_set_style_text_color(cook_fan_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(cook_fan_label, &kaiTI, 0);
    lv_obj_align(cook_fan_label, LV_ALIGN_CENTER, 0, 60);

    // ── 停止按钮 ──
    btn_stop_cook = lv_button_create(scr_cooking);
    lv_obj_set_size(btn_stop_cook, 90, 45);
    lv_obj_align(btn_stop_cook, LV_ALIGN_BOTTOM_MID, +50, -20);
    lv_obj_set_style_bg_color(btn_stop_cook, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_color(btn_stop_cook, lv_color_hex(0xCC0000), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_stop_cook, 12, 0);
    lv_obj_add_event_cb(btn_stop_cook, cook_stop_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * label_stop_cook = lv_label_create(btn_stop_cook);
    lv_label_set_text(label_stop_cook, "停止");
    lv_obj_set_style_text_color(label_stop_cook, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_stop_cook, &kaiTI, 0);
    lv_obj_center(label_stop_cook);

    // =========== 5. 烹饪完成界面 scr_complete ===========
    // 大图标或标题
    lv_obj_t * complete_title = lv_label_create(scr_complete);
    lv_label_set_text(complete_title, "烹饪完成");
    lv_obj_set_style_text_color(complete_title, lv_color_hex(0x006aff), 0);
    lv_obj_set_style_text_font(complete_title, &kaiTI, 0);
    lv_obj_align(complete_title, LV_ALIGN_CENTER, 0, -40);

    // 提示文字
    lv_obj_t * complete_hint = lv_label_create(scr_complete);
    lv_label_set_text(complete_hint, "享受美食吧");
    lv_obj_set_style_text_font(complete_hint, &kaiTI, 0);
    lv_obj_set_style_text_color(complete_hint, lv_color_hex(0x333333), 0);
    lv_obj_align(complete_hint, LV_ALIGN_CENTER, 0, 10);

    // 返回主页按钮
    lv_obj_t * btn_home = lv_button_create(scr_complete);
    lv_obj_set_size(btn_home, 140, 50);
    lv_obj_align(btn_home, LV_ALIGN_CENTER, 0, 60);
    // #ff0000 是红色， #187600 是深绿色
    lv_obj_set_style_bg_color(btn_home, lv_color_hex(0x187600), 0);
    lv_obj_add_event_cb(btn_home, complete_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label_home = lv_label_create(btn_home);
    lv_label_set_text(label_home, "返回主页");
    lv_obj_set_style_text_font(label_home, &kaiTI, 0);
    lv_obj_center(label_home);


    // =========== 5. 载入默认界面 ===========
    lv_scr_load(scr_main);

    lv_timer_create(time_update_timer_cb, 30000, NULL); // 每 30 秒更新一次时间显示

    _lock_release(&lvgl_api_lock);
}

// 提供一个接口让 app_task.c 可以更新 Wi-Fi 状态显示
void ui_wifi_up(WIFI_state_t state)
{
    _lock_acquire(&lvgl_api_lock);
    switch (state) {
        case WIFI_STATE_PROVISIONING:
            // 配网模式，显示黄色
            if (wifi_icon) {
                show_qrcode_panel(); // 同时弹出二维码面板
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xFFA500), 0); // 橙色表示配网中
            }
            break;
        case WIFI_STATE_CONNECTED:
            // 已连接，显示绿色
            if (wifi_icon) {
                 if (qr_panel != NULL) {
                    lv_obj_delete(qr_panel);
                    qr_panel = NULL;
                    ui_qrcode = NULL; // 面板被删，内部的二维码也一并被销毁了
                }
                // #2732ff 是绿色，表示已连接
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x2732ff), 0); // 绿色表示已连接
            }
            break;
        case WIFI_STATE_DISCONNECTED:
            // 断开连接，显示红色
            if (wifi_icon) {
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xFF0000), 0); // 红色表示断开
            }
            break;
        default:
            break;
    }
     _lock_release(&lvgl_api_lock);
}

// 温度和剩余时间更新函数（更新烹饪界面）
void ui_up_temp(float temp, int rem_time_s )
{
    _lock_acquire(&lvgl_api_lock);

    // ✅ 只在烹饪界面活跃时才刷新
    if (lv_screen_active() == scr_cooking && cook_temp_label != NULL && cook_time_label != NULL) {
        if (temp < 100.0f) {
            lv_label_set_text_fmt(cook_temp_label, "#006aff %.1f °C#", temp);
        } else {
            lv_label_set_text_fmt(cook_temp_label, "#ff0000 %.0f °C#", temp);
        }
        lv_label_set_text_fmt(cook_time_label, "剩余 %02d:%02d", rem_time_s / 60, rem_time_s % 60);
    }

    _lock_release(&lvgl_api_lock);
}

//麦克风状态更新接口
void ui_mic_state_update(mic_state_t state)
{
    if (mic_icon == NULL) return;
    _lock_acquire(&lvgl_api_lock);
    switch (state) {
        case MIC_STATE_LISTENING:
            // 未说话状态：显示黑色 #000000
            lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x000000), 0);
            break;
        case MIC_STATE_SPEAKING:
            // 正在说话/录音状态：显示绿色，或者你也可以在这里换个符号！
            lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x00FF00), 0);
            break;
        default:
            break;
    }
    _lock_release(&lvgl_api_lock);
}

// ✅ 公开函数：供 app_task.c 在烹饪结束时调用
void ui_show_cooking_complete(void)
{
    _lock_acquire(&lvgl_api_lock);
    if (scr_complete != NULL) {
        lv_scr_load(scr_complete);
    }
    _lock_release(&lvgl_api_lock);
}

// ✅ 云端菜谱入口：复用 scr_detail，只改标题和预设值
void ui_show_cloud_detail(cloud_data_t *data)
{
    if (data == NULL) return;

    _lock_acquire(&lvgl_api_lock);

    // 1. 将云端数据同步到 current_config
    current_config.temperature = data->temperature;
    current_config.time_s      = data->time_s;
    current_config.fan_speed   = data->fan_speed;
    snprintf(current_config.food_name, sizeof(current_config.food_name), "%s", data->food_name);

    // 2. 更新 scr_detail 上的显示值
    lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
    lv_label_set_text_fmt(label_set_time, "%d min", (int)current_config.time_s / 60);
    lv_label_set_text_fmt(label_set_fan, "Fan: %s",
        current_config.fan_speed == fan_high ? "High" :
        current_config.fan_speed == fan_mid  ? "Mid"  : "Low");
    lv_label_set_text_fmt(label_set_food, "%s", data->food_name);

    // 切换到详细界面
    lv_scr_load(scr_detail);

    _lock_release(&lvgl_api_lock);
}

// ✅ 公开函数：供 app_task.c 云端命令直接触发启动（等同于按下 START）
void ui_cloud_start(void)
{
    _lock_acquire(&lvgl_api_lock);

    cook_config_t cfg = {
        .temperature = current_config.temperature,
        .time_s      = current_config.time_s,
        .fan_speed   = current_config.fan_speed,
        .food_name   = "",
    };
    snprintf(cfg.food_name, sizeof(cfg.food_name), "%s", current_config.food_name);
    esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_aircook,
                      &cfg, sizeof(cfg), 0);

    // ✅ 同步信息到烹饪界面
    if (cook_food_label != NULL)
        lv_label_set_text(cook_food_label, lv_label_get_text(label_set_food));
    if (cook_temp_label != NULL)
        lv_label_set_text_fmt(cook_temp_label, "#006aff %.1f °C#", current_config.temperature);
    if (cook_time_label != NULL)
        lv_label_set_text_fmt(cook_time_label, "剩余 %d:%02d",
            (int)current_config.time_s / 60, (int)current_config.time_s % 60);
    if (cook_fan_label != NULL) {
        const char *fan_str =
            current_config.fan_speed == fan_high ? "High" :
            current_config.fan_speed == fan_mid  ? "Mid"  : "Low";
        lv_label_set_text_fmt(cook_fan_label, "风速  %s", fan_str);
    }

    // ✅ 切换到烹饪界面
    lv_scr_load(scr_cooking);
    _lock_release(&lvgl_api_lock);
}
