#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(AIR_COOKER_EVENTS);

// 定义所有的事件 ID (动词：谁让系统干嘛，或者系统发生了什么)
typedef enum {
    EVENT_CMD_FAN,        // 指令：开始工作
    EVENT_CMD_STOP,         // 指令：停止工作
    EVENT_CMD_SET_TEMP,     // 指令：设置目标温度
    EVENT_TEMP_UPDATED,     // 状态：当前实际温度更新了 (用来通知屏幕刷新数字)
} air_cooker_event_id_t;

// 定义附带的数据结构 (传温度时用到)
typedef struct {
    uint32_t temperature;
    uint32_t SPEED;
    uint32_t time_mins;
} cook_config_t;

