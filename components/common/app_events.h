#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(AIR_COOKER_EVENTS);

// LCD引脚定义
#define PIN_BK_LIGHT GPIO_NUM_41
#define PIN_NUM_SCLK GPIO_NUM_1
#define PIN_NUM_MOSI GPIO_NUM_0
#define PIN_NUM_MISO GPIO_NUM_21
#define PIN_NUM_LCD_DC GPIO_NUM_2
#define PIN_NUM_LCD_CS GPIO_NUM_19
#define PIN_NUM_LCD_RST GPIO_NUM_47
#define PIN_NUM_TOUCH_CS GPIO_NUM_42

// NTC ADC引脚定义
#define NTC_AD GPIO_NUM_6 // ADC_CHANNEL_5

// V220_CON引脚定义
#define HERT_CON_GPIO GPIO_NUM_13
#define FAN_CON_GPIO GPIO_NUM_11
#define ZERO_CROSS GPIO_NUM_5

// 定义所有的事件 ID (动词：谁让系统干嘛，或者系统发生了什么)
typedef enum {
    EVENT_CMD_FAN,        // 指令：开始工作
    EVENT_CMD_HOT,         // 指令：停止工作
    EVENT_CMD_SET_TEMP,     // 指令：设置目标温度
    EVENT_TEMP_UPDATED,     // 状态：当前实际温度更新了 (用来通知屏幕刷新数字)
} air_cooker_event_id_t;

// 定义附带的数据结构 (传温度时用到)
typedef struct {
    float temperature;
    uint32_t SPEED;
    uint32_t time_mins;
} cook_config_t;

extern esp_event_loop_handle_t loop_handle;
