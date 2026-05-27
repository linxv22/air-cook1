#pragma once

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

//I2C引脚定义
#define ES_I2C_SDA GPIO_NUM_17
#define ES_I2C_CLK GPIO_NUM_18

//I2S引脚定义
#define Codec_I2S0_DSDIN GPIO_NUM_8
#define Codec_I2S0_MCLK GPIO_NUM_16
#define Codec_I2S0_SCLK GPIO_NUM_9
#define Codec_I2S0_LRCK GPIO_NUM_45

//ES7210引脚定义
#define ES7210_SDOUT GPIO_NUM_10

//PA引脚定义
#define PA_CTRL GPIO_NUM_48

//micro sd卡引脚定义
#define ESP0_SD_CLK GPIO_NUM_15
#define ESP0_SD_CMD GPIO_NUM_7
#define ESP0_SD_DAT0 GPIO_NUM_4

extern esp_event_loop_handle_t loop_handle;

// 定义所有的事件 ID 
typedef enum {
    EVENT_CMD_aircook = 0,      // 指令：开始工作
    EVENT_CMD_SET_TEMP,         // 指令：设置目标温度
    EVENT_CMD_FAN_SPEED,        // 指令：设置风扇速度
    EVENT_CMD_STOP,             // 指令：停止工作
    //状态更新事件：底层状态机发生了变化，通知 UI 刷新显示
    EVENT_TEMP_UPDATED,         // 状态：当前实际温度更新了 (用来通知屏幕刷新数字)
    EVENT_WIND_UPDATED,         // 状态：当前显示页面更新了
    //WIFI事件更新
    EVENT_QR_CODE_READY,        // Wi-Fi DPP事件：QR Code准备好了，快去扫码连接吧
    EVENT_WIFI_CONNECTED,       // Wi-Fi DPP事件：成功连接Wi-Fi了
    EVENT_WIFI_DISCONNECTED,    // Wi-Fi事件：Wi-Fi断开了
    //音频事件更新
    EVENT_AUDIO_CMD,    // 音频事件：检测到说话了
    //云端事件更新
    EVENT_CLOUD_CMD, // 云端事件：成功连接云端了
} air_cooker_event_id_t;

typedef enum {
    wind_main,   //ui主界面
    wind_working,  //ui烹饪界面
}wind_state_t;

typedef enum {
    fan_high = 0, 
    fan_mid,
    fan_low,
}fan_speed_t;

// 定义烹饪事件结构体 (传命令到底层用得到)
typedef struct {
    float temperature;//设定温度
    uint32_t time_s;//设定烹饪时间
    fan_speed_t fan_speed;//设定风扇速度
} cook_config_t;

//设备wifi状态结构体
typedef enum{
    WIFI_STATE_INIT = 0,       // 初始状态 / 准备中
    WIFI_STATE_CONNECTED,      // 已成功连接并获取到 IP
    WIFI_STATE_DISCONNECTED,   // 断开连接（正在后台自动重连）
    WIFI_STATE_PROVISIONING    // 处于配网模式（正在等待扫码）
} WIFI_state_t;
