#ifndef V220_CON_H
#define V220_CON_H

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" 
#include "driver/gpio.h"
#include "esp_intr_types.h"
#include "app_events.h"

typedef enum {
    cook_stopped=0,
    cook_running,
    cook_paused,
    cook_cooling_down,
    cook_error,
} cook_state_t;

typedef struct {
    float temperature;// 目标温度
    uint32_t SPEED; // 风扇转速，0-100
    uint32_t time_s;// 烹饪剩余时间，单位秒
    cook_state_t state; // 当前烹饪状态
} run_config_t;

// 220v相关电路底层初始化
void v220_con_init(void);
//启动烹饪过程
void aircook_start(cook_config_t *config);
//设定温度
void aircook_set_tem(float tem);
//获取剩余烹饪时间
uint32_t aircook_gettime(void);
//停止烹饪过程
void aircook_stop(void);
//设定风扇转速
void aircook_set_speed(fan_speed_t speed);


// Your header file content here

#endif // V220_CON_H