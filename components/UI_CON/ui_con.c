#include "ui_con.h"

#include "lvgl.h"
#include "esp_log.h"
#include <time.h> 


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern _lock_t lvgl_api_lock;
extern WIFI_state_t WIFI_STATE;

static const char *TAG = "UI_CON";

// ============ UI 局部状态缓存 ============ 
static cook_config_t current_config = {
    .temperature = 180.0f,
    .time_s = 15 * 60,
    .fan_speed = fan_mid
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
    BTN_ID_START,// 开始烹饪
    BTN_ID_STOP// 停止烹饪
} btn_id_t;

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
                cook_config_t cfg = {
                    .temperature = current_config.temperature,
                    .time_s = current_config.time_s
                };
                esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_aircook, &cfg, sizeof(cfg), 0);
                break;
            
            case BTN_ID_STOP:
                esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_STOP, NULL, 0, 0);
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
        } else if (btn_id == BTN_ID_PRESET_CHICKEN) {
            current_config.temperature = 200.0f;
            current_config.time_s = 25 * 60;
            current_config.fan_speed = fan_high;
        } else if (btn_id == BTN_ID_PRESET_STEAK) {
            current_config.temperature = 200.0f;
            current_config.time_s = 12 * 60;
            current_config.fan_speed = fan_high;
        }
        
        // 更新详细界面的 Label 值
        lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
        lv_label_set_text_fmt(label_set_time, "%d min", (int)current_config.time_s / 60);
        lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", (int)(current_config.fan_speed * 100));
        
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
    //若当前未连接 Wi-Fi，且配网链接不为空，则再次呼出面板
    if (WIFI_STATE != WIFI_STATE_CONNECTED) {
        show_qrcode_panel();
    }
}

// 辅助创建按钮包装器
static lv_obj_t * create_ui_btn(lv_obj_t * parent, const char * txt, int x, int y, btn_id_t btn_id)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_center(label);
    
    // 设置位置和点击事件
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    // 强制转换为无类型指针发送
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)btn_id);
    return btn;
}


// ============ UI 相关函数实现 ============
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
    lv_obj_add_event_cb(btn_fries, preset_btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)BTN_ID_PRESET_FRIES);
    lv_obj_t * label_fries = lv_label_create(btn_fries);
    lv_label_set_text(label_fries, "Fries");
    lv_obj_center(label_fries);

    lv_obj_t * btn_chicken = lv_button_create(scr_main);
    lv_obj_set_size(btn_chicken, 100, 40);
    lv_obj_align(btn_chicken, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_chicken, preset_btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)BTN_ID_PRESET_CHICKEN);
    lv_obj_t * label_chicken = lv_label_create(btn_chicken);
    lv_label_set_text(label_chicken, "Chicken");
    lv_obj_center(label_chicken);

    lv_obj_t * btn_steak = lv_button_create(scr_main);
    lv_obj_set_size(btn_steak, 100, 40);
    lv_obj_align(btn_steak, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_event_cb(btn_steak, preset_btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)BTN_ID_PRESET_STEAK);
    lv_obj_t * label_steak = lv_label_create(btn_steak);
    lv_label_set_text(label_steak, "Steak");
    lv_obj_center(label_steak);

    // =========== 4. 绘制详细界面 (scr_detail) ===========
    // 状态显示区
    Tem_label = lv_label_create(scr_detail);
    // 等到 LVGL 9 才可以用 lv_label_set_text_fmt 直接写带颜色的标签，使用旧版格式化注意
    // 或者直接使用文本拼接：
    // lv_label_set_recolor(Tem_label, true); // 此函数在 LVGL v8 被废除，转而使用 style 
    // 若原库是 v8 ，您这行可能编译失败或有效，这里保持您原带的样子。
    lv_label_set_recolor(Tem_label, true); 
    lv_label_set_text(Tem_label, "Cur Tem: #006aff 25.0 °C#");
    lv_obj_align(Tem_label, LV_ALIGN_TOP_LEFT, 10, 45); 

    Remain_time_label = lv_label_create(scr_detail);
    lv_label_set_text(Remain_time_label, "Rem: 00:00");
    lv_obj_align(Remain_time_label, LV_ALIGN_TOP_RIGHT, -10, 45);

    /* 参数调控区：这里已经全面更改为枚举传递 */
    create_ui_btn(scr_detail, "-", -70, -50, BTN_ID_TEMP_MINUS);
    label_set_temp = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_temp, "%d °C", (int)current_config.temperature);
    lv_obj_align(label_set_temp, LV_ALIGN_CENTER, 0, -50);
    create_ui_btn(scr_detail, "+", 70, -50, BTN_ID_TEMP_PLUS);

    create_ui_btn(scr_detail, "-", -70, 0, BTN_ID_TIME_MINUS);
    label_set_time = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_time, "%d min",(int) current_config.time_s / 60);
    lv_obj_align(label_set_time, LV_ALIGN_CENTER, 0, 0);
    create_ui_btn(scr_detail, "+", 70, 0, BTN_ID_TIME_PLUS);

    create_ui_btn(scr_detail, "-", -70, 50, BTN_ID_FAN_MINUS);
    label_set_fan = lv_label_create(scr_detail);
    lv_label_set_text_fmt(label_set_fan, "Fan: %d%%", (int)(current_config.fan_speed * 100));
    lv_obj_align(label_set_fan, LV_ALIGN_CENTER, 0, 50);
    create_ui_btn(scr_detail, "+", 70, 50, BTN_ID_FAN_PLUS);

    /* 底部操作区 */
    lv_obj_t* btn_start = create_ui_btn(scr_detail, "START", -50, 100, BTN_ID_START);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x187600), 0); 
    lv_obj_t* btn_stop = create_ui_btn(scr_detail, "STOP", 50, 100, BTN_ID_STOP);
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
                lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x00FF00), 0); // 绿色表示已连接
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

// 提供一个接口让 app_task.c 可以更新当前温度显示
void ui_up_temp(float temp, int rem_time_s)
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[16];
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    
    _lock_acquire(&lvgl_api_lock);
    
    // ++ 必须做这步防御性判断！如果详情页都没有显示出来，就不应该去刷新详情页里的 Label！++
    if (lv_screen_active() == scr_detail && Tem_label != NULL) {
        lv_label_set_text_fmt(Tem_label, "Cur Tem: #006aff %.1f °C#", temp);
        lv_label_set_text_fmt(Remain_time_label, "Rem: %02d:%02d", rem_time_s / 60, rem_time_s % 60);
    }
    
    if (time_label != NULL) {
        lv_label_set_text(time_label, strftime_buf);
    }
    
    _lock_release(&lvgl_api_lock);
}
