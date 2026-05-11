#ifndef V220_CON_H
#define V220_CON_H

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" 
#include "driver/gpio.h"
#include "esp_intr_types.h"

typedef enum {
    cook_stopped=0,
    cook_running,
    cook_paused,
    cook_run_stop,
    cook_error,
} cook_state_t;

typedef struct {
    float temperature;// 目标温度
    uint32_t SPEED; // 风扇转速，0-100
    uint32_t time_s;// 烹饪剩余时间，单位秒
    cook_state_t state; // 当前烹饪状态
} run_config_t;

// Function prototypes for V220_CON component
void v220_con_init(void);
void V220_HOT_CON(bool con);
void V220_FAN_CON(bool con ,uint32_t speed);
// Your header file content here

#endif // V220_CON_H