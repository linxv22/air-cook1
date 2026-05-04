#include "lvgl.h"
#include "LCD.h"
#include "app_events.h"
#include "stdio.h"
#include "string.h"
#include <sys/lock.h>
#include "esp_log.h"

extern _lock_t lvgl_api_lock;
extern esp_event_loop_handle_t loop_handle;
extern cook_config_t cook_config;

static const char *TAG = "flush";

static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    // ESP_LOGI(TAG, "Button event code: %d", code);
    // 获取传递过来的 user_data，这里是刚刚传进来的字符串
    const char * btn_id = lv_event_get_user_data(e); 
    static uint8_t cnt = 0;
    lv_obj_t * btn = lv_event_get_target(e);
    if(code == LV_EVENT_RELEASED) {
        if (strcmp(btn_id, "FAN-") == 0) {
            // 处理 FAN_CON 按钮被按下的逻辑
            if(cnt == 0) {
                cnt = 100;
            }
            cnt -= 10;
            
            cook_config.SPEED = cnt; // 更新全局的风扇转速配置
             /*Get the first child of the button which is the label and change its text*/
            lv_obj_t * label = lv_obj_get_child(btn, 0);
            lv_label_set_text_fmt(label, "FAN+: %d", cnt);
            // ESP_LOGI(TAG, "FAN_CON button pressed, speed: %d", cnt);
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN, NULL, 0, 1000 / portTICK_PERIOD_MS); // 发送事件通知主任务风扇转速更新了
        } else if (strcmp(btn_id, "FAN+") == 0) {
            // 处理 FAN_CON 按钮被按下的逻辑
            
            cnt+=10;
            if(cnt>100)
            cnt = 0;

            cook_config.SPEED = cnt; // 更新全局的风扇转速配置
             /*Get the first child of the button which is the label and change its text*/
            lv_obj_t * label = lv_obj_get_child(btn, 0);
            lv_label_set_text_fmt(label, "FAN+: %d", cnt);
            // ESP_LOGI(TAG, "FAN_CON button pressed, speed: %d", cnt);
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN, NULL, 0, 1000 / portTICK_PERIOD_MS); // 发送事件通知主任务风扇转速更新了
        }
    }

}

void example_lvgl_demo_ui(void)
{
    
    lv_obj_t * cz_label = lv_label_create(lv_screen_active());
    lv_obj_set_width(cz_label, 150);
    lv_obj_align(cz_label, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_recolor(cz_label,true);                     /*Enable re-coloring by commands in the text*/
    lv_label_set_text(cz_label, 
                     "#0000ff fan# #00ff00 open#");
    lv_obj_t * label2 = lv_label_create(lv_screen_active());
    lv_obj_set_width(label2, 150);
    lv_label_set_recolor(label2,true);                     /*Enable re-coloring by commands in the text*/
    lv_label_set_text(label2, 
                     "#0000ff fan# #ff0000 close#");
    lv_obj_align(label2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * btn = lv_button_create(lv_screen_active());     /*Add a button the current screen*/
    // lv_obj_set_pos(btn, 10, 10);                            /*Set its position*/
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 100);    
    lv_obj_set_size(btn, 120, 50);                          /*Set its size*/
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_ALL, NULL);           /*Assign a callback to the button*/

    lv_obj_t * label = lv_label_create(btn);          /*Add a label to the button*/
    lv_label_set_text(label, "Button");                     /*Set the labels text*/
    lv_obj_center(label);


}



void START_air_cook(void)
{
    _lock_acquire(&lvgl_api_lock);

    /* 1. 创建顶部大字标题：AIR cook */
    lv_obj_t * title_label = lv_label_create(lv_screen_active());
    lv_label_set_text(title_label, "AIR cook");
    
    /* 设置较大的内置字体。注意：请确保 lv_conf.h 中已经启用了所选的字体，如 LV_FONT_MONTSERRAT_24 */
     lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0); 
    
    /* 顶部居齐，向下偏移 40 像素 */
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 0); 


    /* 2. 创建第一个按钮：HOT_CON (居中偏左) */
    lv_obj_t * btn_hot = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn_hot, 100, 50);
    lv_obj_align(btn_hot, LV_ALIGN_CENTER, -70, 0); /* X 轴向左偏移 70 */
    
    /* 给 HOT_CON 按钮添加标签 */
    lv_obj_t * label_hot = lv_label_create(btn_hot);
    lv_label_set_text(label_hot, "FAN-:0");
    lv_obj_center(label_hot);

    /* 3. 创建第二个按钮：FAN_CON (居中偏右) */
    lv_obj_t * btn_fan = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn_fan, 100, 50);
    lv_obj_align(btn_fan, LV_ALIGN_CENTER, 70, 0);  /* X 轴向右偏移 70 */
    
    /* 给 FAN_CON 按钮添加标签 */
    lv_obj_t * label_fan = lv_label_create(btn_fan);
    lv_label_set_text(label_fan, "FAN+:0");
    lv_obj_center(label_fan);

    lv_obj_add_event_cb(btn_hot, btn_event_cb, LV_EVENT_ALL, "FAN-");
    lv_obj_add_event_cb(btn_fan, btn_event_cb, LV_EVENT_ALL, "FAN+");

    _lock_release(&lvgl_api_lock);
}
