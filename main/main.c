#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
#include "app_events.h"

static const char *TAG = "main";

esp_event_loop_handle_t loop_handle;
cook_config_t cook_config = {
    .temperature = 0, // 目标温度，单位摄氏度
    .SPEED = 0,        // 风扇转速，范围 0-100
    .time_mins = 0     // 烹饪时间，单位分钟
};

ESP_EVENT_DEFINE_BASE(AIR_COOKER_EVENTS);
static void app_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{

    ESP_LOGI(TAG, "Received event: base=%s, id=%d", base, id);
    switch (id) {
        case EVENT_CMD_FAN:
            ESP_LOGI(TAG, "Handling EVENT_CMD_FAN %d", cook_config.SPEED);
            V220_FAN_CON(true, cook_config.SPEED);
            break;
        case EVENT_CMD_STOP:
            ESP_LOGI(TAG, "Handling EVENT_CMD_STOP");
            break;
        case EVENT_CMD_SET_TEMP:
            ESP_LOGI(TAG, "Handling EVENT_CMD_SET_TEMP");
            break;
        case EVENT_TEMP_UPDATED:
            ESP_LOGI(TAG, "Handling EVENT_TEMP_UPDATED");
            break;
    }


}


void app_event_init (void);

void app_main(void)
{

ESP_LOGI(TAG, "Starting NTC ADC example");

    // Initialize the NTC ADC
    ntc_adc_init();

    // Initialize the V220_CON component
    v220_con_init();

    // Initialize the LCD display
    LCD_Init();

    app_event_init();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "Main loop running...");
    
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

