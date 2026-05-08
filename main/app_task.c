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

cook_config_t cook_config = {
    .temperature = 25.0, // 目标温度，单位摄氏度
    .SPEED = 0,        // 风扇转速，范围 0-100
    .time_mins = 0     // 烹饪时间，单位分钟
};

bool is_HOT_ON = false;

lv_obj_t * label_hot;
lv_obj_t * label_fan;
lv_obj_t * Tem_label;
lv_obj_t * label_test_hot;

void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{

    // ESP_LOGI(TAG, "Received event: base=%s, id=%d", base, id);
    switch (id) {
        case EVENT_CMD_FAN:
            ESP_LOGI(TAG, "Handling EVENT_CMD_FAN %d", cook_config.SPEED);
            if(cook_config.SPEED > 0) {
                V220_FAN_CON(true, cook_config.SPEED);
            } else {
                V220_FAN_CON(false, 0);
            }
            if (label_hot != NULL && label_fan != NULL) {
                _lock_acquire(&lvgl_api_lock);
                // 使用 lv_label_set_text_fmt 动态格式化数字
                lv_label_set_text_fmt(label_hot, "FAN-:%d", (int)cook_config.SPEED);
                lv_label_set_text_fmt(label_fan, "FAN+:%d", (int)cook_config.SPEED);
                _lock_release(&lvgl_api_lock);
            }
            break;
        case EVENT_CMD_HOT:
            ESP_LOGI(TAG, "Handling EVENT_CMD_HOT");
            is_HOT_ON = !is_HOT_ON;
            // V220_HOT_CON(is_HOT_ON);
            if(is_HOT_ON)
            {
                _lock_acquire(&lvgl_api_lock);
                lv_label_set_text(label_test_hot, "HOT ON");
                _lock_release(&lvgl_api_lock);
            }
             else
            {
                _lock_acquire(&lvgl_api_lock);
                lv_label_set_text(label_test_hot, "HOT OFF");
                _lock_release(&lvgl_api_lock);
            }
            break;
        case EVENT_TEMP_UPDATED:
            // ESP_LOGI(TAG, "Handling EVENT_TEMP_UPDATED: %.1f C", cook
            _lock_acquire(&lvgl_api_lock);
            lv_label_set_text_fmt(Tem_label, "Tem: %.1f °C", ntc_adc_read_temperature());
            _lock_release(&lvgl_api_lock);
            break;
    }

}



void app_event_init (void)
{

    // 2. 定义事件循环参数。此处创建了一个专门的事件循环任务来处理事件，队列大小为 5，任务优先级为 5，栈大小为 4096 字节。
    esp_event_loop_args_t loop_args = {
        .queue_size = 10,
        .task_name = "my_event_task", // task will be created
        .task_priority = 5,
        .task_stack_size = 4096,
        .task_core_id = tskNO_AFFINITY
    };

    // Create the event loops
    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &loop_handle));

    // 使用刚才得到的句柄（air_cooker_loop_handle），注册我们的事件监听器
    ESP_ERROR_CHECK(esp_event_handler_register_with(
        loop_handle, // 指向你要监听的特定循环基底
        AIR_COOKER_EVENTS,      // 从 app_events.h 里约定的暗号（基底）
        ESP_EVENT_ANY_ID,       // 监听此基底下的所有 ID
        app_event_handler,      // 执行的回调函数
        NULL                    // 不传递额外参数
    ));


}



static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    // ESP_LOGI(TAG, "Button event code: %d", code);
    // 获取传递过来的 user_data，这里是刚刚传进来的字符串
    const char * btn_id = lv_event_get_user_data(e); 
    if(code == LV_EVENT_RELEASED) {
        if (strcmp(btn_id, "FAN-") == 0) {
            
            if (cook_config.SPEED == 0) {
                cook_config.SPEED = 100;
            }

            cook_config.SPEED--; // 更新全局的风扇转速配置
             /*Get the first child of the button which is the label and change its text*/
            // ESP_LOGI(TAG, "FAN_CON button pressed, speed: %d", cnt);
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN, NULL, 0, 1000 / portTICK_PERIOD_MS); // 发送事件通知主任务风扇转速更新了
        } else if (strcmp(btn_id, "FAN+") == 0) {
            // 处理 FAN_CON 按钮被按下的逻辑
            cook_config.SPEED++; // 更新全局的风扇转速配置
            if (cook_config.SPEED >=100) {
                cook_config.SPEED = 0;
            }
             /*Get the first child of the button which is the label and change its text*/
            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FAN, NULL, 0, 1000 / portTICK_PERIOD_MS); // 发送事件通知主任务风扇转速更新了

        } else if (strcmp(btn_id, "TEST HOT") == 0) {
           esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_HOT, NULL, 0, 1000 / portTICK_PERIOD_MS); // 发送事件通知主任务风扇转速更新了
             /*Get the first child of the button which is the label and change its text*/   
        }
    }

}


void ui_staret(void)
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
    lv_obj_align(btn_hot, LV_ALIGN_CENTER, -60, -40); /* X 轴向左偏移 70 */
    
    /* 给 HOT_CON 按钮添加标签 */
    label_hot = lv_label_create(btn_hot);
    lv_label_set_text(label_hot, "FAN-:0");
    lv_obj_center(label_hot);

    /* 3. 创建第二个按钮：FAN_CON (居中偏右) */
    lv_obj_t * btn_fan = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn_fan, 100, 50);
    lv_obj_align(btn_fan, LV_ALIGN_CENTER, 60, -40);  /* X 轴向右偏移 70 */
    
    /* 给 FAN_CON 按钮添加标签 */
    label_fan = lv_label_create(btn_fan);
    lv_label_set_text(label_fan, "FAN+:0");
    lv_obj_center(label_fan);

    lv_obj_add_event_cb(btn_hot, btn_event_cb, LV_EVENT_ALL, "FAN-");
    lv_obj_add_event_cb(btn_fan, btn_event_cb, LV_EVENT_ALL, "FAN+");

    Tem_label = lv_label_create(lv_screen_active());
    lv_label_set_text_fmt(Tem_label, "Tem: %.1f °C",25.0f);
    // ESP_LOGI(TAG, "Initial temperature: %.1f °C", cook_config.temperature);
    lv_obj_align(Tem_label, LV_ALIGN_LEFT_MID, 0, -100); /* 底部居中，向上偏移 20 */

    lv_obj_t * test_hot= lv_button_create(lv_screen_active());
    lv_obj_set_size(test_hot, 100, 50);
    lv_obj_align(test_hot, LV_ALIGN_CENTER, -60, 40); /* X 轴向左偏移 70 */
    label_test_hot = lv_label_create(test_hot);
    lv_label_set_text(label_test_hot, "HOT OFF");
    lv_obj_center(label_test_hot);
    lv_obj_add_event_cb(test_hot, btn_event_cb, LV_EVENT_ALL, "TEST HOT");

    _lock_release(&lvgl_api_lock);
}
