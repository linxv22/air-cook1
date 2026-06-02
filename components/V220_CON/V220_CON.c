#include "V220_CON.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "NTC_ADC.h"



#define TAG "V220_CON"



// ================= 全局控制变量 =================
volatile static bool is_fan_on = false;            
volatile static uint32_t delay_time_us = 1000;     // 控制转速，范围 0-9900

static rmt_channel_handle_t rmt_tx_chan = NULL;
static rmt_encoder_handle_t rmt_copy_encoder = NULL;
static TaskHandle_t rmt_tx_task_handle = NULL;

static run_config_t current_config = {
    .temperature = 180.0f,
    .SPEED = 60,
    .time_s = 15 * 60,
    .state = cook_stopped
}; // 当前烹饪配置

//函数声明
static void V220_HOT_CON(bool con);
static void V220_FAN_CON(bool con ,uint32_t speed);

// ================= 1. ISR 过零中断 =================
static void IRAM_ATTR zcd_isr_handler(void* arg)
{
    
    BaseType_t high_task_wakeup = pdFALSE;
    // 【核心修复：15毫秒软件防抖，滤除所有硬件毛刺】
    int64_t current_time = esp_timer_get_time(); // 获取系统当前运行时间(微秒)
    static int64_t last_trigger_time = 0;
    
    // 50Hz 周期为 20000us，如果在 15000us 内再次触发，绝对是杂波，直接丢弃！
    if (current_time - last_trigger_time < 15000) {
        return; 
    }
    last_trigger_time = current_time; // 记录本次真实的过零时间
    // 发送信号量，通知高优先级任务立刻执行发送
    vTaskNotifyGiveFromISR(rmt_tx_task_handle, &high_task_wakeup);
    
    // 如果唤醒的任务比当前正在运行的任务优先级高，立刻发起 CPU 调度
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// ================= 2. 高优先级 RMT 调度任务 =================
static void rmt_tx_task(void *arg)
{
     rmt_transmit_config_t tx_config = {
            .loop_count = 0, // 仅发一遍序列
            .flags.eot_level = 0, //发送完设置为低电平
        };
    while (1) {
        // 无限期等待 ISR 发送通知，没有触发时死等，0 占用 CPU
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // 【安全机制】：检查上一次传输是否已经结束？(超时设为0，立刻返回)
        esp_err_t err = rmt_tx_wait_all_done(rmt_tx_chan, 0); 
        // ESP_LOGE(TAG, "receive CNT");
        if (err == ESP_ERR_TIMEOUT) {
            // RMT 硬件显示上一次的波形还在发，说明上一个 20ms 的下半周期还没结束，就又触发了中断！
            // 这往往是高频杂波导致了误触发。为了安全，坚决不发送，直接丢弃本次事件。
            continue;  
        }

        // 组建 2 波形 (前半周期 + 后半周期)
        // RMT 配置为 1MHz，即 1 tick = 1 us
        // 在 ESP-IDF v5 中，rmt_symbol_word_t 包含了 duration0, level0, duration1, level1 这四个位域属性
        rmt_symbol_word_t symbols[2] = {
            // 阶段1：先输出低(0)电平 delay_time_us 微秒，再输出高(1)电平 1000 微秒
            { .duration0 = delay_time_us, .level0 = 0, .duration1 = 1000, .level1 = 1 },
            
            // 阶段2：空闲低电平约 10ms-200us=9800us 等待下半周，到位后再打出 200us 高脉冲
            { .duration0 = 9000,          .level0 = 0, .duration1 = 1000, .level1 = 1 }
        };

        // 推送到硬件 (不阻塞 CPU，几微秒后立刻返回！)
        rmt_transmit(rmt_tx_chan, rmt_copy_encoder, symbols, sizeof(symbols), &tx_config);
    }
}

//空气炸锅底层控制中枢
static void cook_control(void *arg)
{
    //空气炸锅散热时间
    uint32_t post_cook_cooling_s = 0;
     // xLastWakeTime 记录上次唤醒的精确 Tick
    TickType_t xLastWakeTime = xTaskGetTickCount();
    float current_temp = 0.0f;

     // 专门为继电器准备的保护变量
    uint32_t relay_lock_sec = 0; // 继电器动作锁定倒计时(秒)
    bool target_relay_state = false; // 记录继电器当前的目标状态
    while (1){    
    current_temp = ntc_adc_read_temperature();
    if(current_temp >=300.0f || current_temp <= -20.0f)
    current_config.state = cook_error;
    switch (current_config.state)
    {
        case cook_stopped:
            V220_HOT_CON(false);
            V220_FAN_CON(false, 0);
            break;
        case cook_running:
            if (current_config.time_s > 0)
            {
                current_config.time_s--;
            }
            if (current_config.time_s == 0)
            {
                current_config.state = cook_cooling_down;
                esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CMD_FINISH,
                                    NULL, 0, 100/portTICK_PERIOD_MS);
                post_cook_cooling_s = 30;
            }
            if (relay_lock_sec > 0) {
                    relay_lock_sec--; // 死区锁定中，不改变继电器状态
                } else {
                    float temp_diff = current_config.temperature - current_temp;

                    if (temp_diff > 30.0f) {
                        // 阶段1【全速升温】：距离目标大于30度，必须吸合加热
                        if (target_relay_state == false) {
                            target_relay_state = true;
                            relay_lock_sec = 20; // 吸合至少保持20秒 (保护触点)
                        }
                    } 
                    else if (temp_diff <= 10.0f ) {
                        // 阶段2【惯性防冲/越界断开】：距离目标还有10度就提前断开，防止余温把锅冲爆
                        if (target_relay_state == true) {
                            target_relay_state = false;
                            relay_lock_sec = 10; // 断开后至少需要等10秒才能再次吸合
                        }
                    } 
                    else if (temp_diff > 10.0f && temp_diff <= 30.0f) {
                        // 阶段3【恒温补偿】：温度跌下来，差了10度以上，启动缓慢补温
                        if (target_relay_state == false) {
                            target_relay_state = true;
                            relay_lock_sec = 10;  // 补气只要短时间吸合即可
                        }
                    }
                }
                // 执行动作
                V220_HOT_CON(target_relay_state);
                V220_FAN_CON(true, current_config.SPEED);
            break;
        case cook_paused:
            V220_HOT_CON(false);
            V220_FAN_CON(true, 60); // 暂停时保持风扇高速运转，帮助散热
            break;
        case cook_cooling_down:
            if (post_cook_cooling_s > 0) 
            { // 每秒更新一次剩余时间
                post_cook_cooling_s--;
            }
            else if (post_cook_cooling_s == 0)
            {
                current_config.state = cook_stopped;
            }
            V220_HOT_CON(false);
            V220_FAN_CON(true,60);
            break;
        case cook_error:
            V220_HOT_CON(false);
            V220_FAN_CON(false, 0);
            break;
        default:
            break;
    }
    xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
   }
}

// ================= 3. 初始化过程 =================
void v220_con_init(void) 
{
    ESP_LOGI(TAG, "Initializing V220_CON with RMT Engine...");

    // (1) 初始化电热丝引脚
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE, 
        .mode = GPIO_MODE_OUTPUT,       
        .pin_bit_mask = (1ULL << HERT_CON_GPIO),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE    
    };
    gpio_config(&io_conf); 
    gpio_set_level(HERT_CON_GPIO, 0); 

    // (2) 初始化 RMT TX 通道与发生器
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT, //选择时钟源
        .gpio_num = FAN_CON_GPIO,       // 风扇驱动引脚托管给 RMT
        .mem_block_symbols = 64,       // 内存块大小，即 64 * 4 = 256 字节
        .resolution_hz = 1 * 1000 * 1000,       // 分辨率 1MHz -> 1 tick = 1us 精确控制
        .trans_queue_depth = 1,       // 设置后台等待处理的事务数量
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &rmt_tx_chan));

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &rmt_copy_encoder));
    ESP_ERROR_CHECK(rmt_enable(rmt_tx_chan));

    // (3) 创建极高优先级 RMT 发送任务 (configMAX_PRIORITIES - 1 为系统最高，不会被LVGL打断)
    xTaskCreatePinnedToCore(
        rmt_tx_task, 
        "rmt_tx_task", 
        2048, 
        NULL, 
        configMAX_PRIORITIES - 1, 
        &rmt_tx_task_handle, 
        1 // 挂在 Core 1，分离网络带来的干扰
    );

    // (4) 配置 ZCD 过零检测输入脚 (下降沿触发)
    gpio_config_t zcd_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ZERO_CROSS),
        .pull_up_en = 1 
    };
    gpio_config(&zcd_conf);

    // (5) 注册过零中断
    // 若原先已经在别处开启了 isr_service 这里忽略 ESP_ERR_INVALID_STATE 即可
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
    if(isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    gpio_isr_handler_add(ZERO_CROSS, zcd_isr_handler, NULL);
    gpio_intr_disable(ZERO_CROSS); // 初始化时先默认关闭硬件中断
    xTaskCreatePinnedToCore(
        cook_control, 
        "cook_control", 
        2048, 
        NULL, 
        8, // 0-25
        NULL, 
        1 // 挂在 Core 1，分离网络带来的干扰
    );

    ESP_LOGI(TAG, "initialized successful!");
}

void aircook_start(cook_config_t *config)
{
    // 更新全局配置时间和温度
    current_config.time_s = config->time_s; 
    current_config.temperature = config->temperature;

    // 根据传入的枚举值确定硬件的实际风速百分比
        switch (config->fan_speed) {
            case fan_high:
                current_config.SPEED = 85; 
                break;
            case fan_mid:
                current_config.SPEED = 70;
                break;
            case fan_low:
                current_config.SPEED = 60;
                break;
            default:
                current_config.SPEED = 60; // 越界保护，默认为低速
                break;
        }
    
    // 切换底层状态机为运行
    current_config.state = cook_running;
}

void aircook_set_tem(float tem)
{
    current_config.temperature = tem;
}

void aircook_set_speed(fan_speed_t speed)
{
     switch (speed) {
            case fan_high:
                current_config.SPEED = 83; 
                break;
            case fan_mid:
                current_config.SPEED = 70;
                break;
            case fan_low:
                current_config.SPEED = 60;
                break;
            default:
                current_config.SPEED = 60; // 越界保护，默认为低速
                break;
        }
}

void aircook_stop(void)
{
    current_config.state = cook_stopped;
}

uint32_t aircook_gettime(void)
{
    return current_config.time_s;
}

// 电热丝控制函数 
// 参数: con: true-开，false-关
static void V220_HOT_CON(bool con)
{
    if(con == true) {
        gpio_set_level(HERT_CON_GPIO, 1);
    } else {
        gpio_set_level(HERT_CON_GPIO, 0);
    }
}

// 风扇控制函数
// 参数: con: true-开，false-关 speed：0-100，表示风扇转速百分比
static void V220_FAN_CON(bool con, uint32_t speed)
{
    if(con == true) {
        if (speed > 100) speed = 100; // 限制 speed 在 0-100 范围内
        delay_time_us = 1000 + (100 - speed) * 80; // 线性映射计算延时时间
        gpio_intr_enable(ZERO_CROSS); 
    } else {
        // 关闭风扇后，我们保证下一次传输能被重置清空电平输出 (防常高电平锁死可控硅)
        rmt_disable(rmt_tx_chan); 
        rmt_enable(rmt_tx_chan); 
        gpio_intr_disable(ZERO_CROSS);
    }
}

cook_state_t aircook_getstate(void)
{
    // ESP_LOGI(TAG, "Current State: %d, Temp: %.1f C, Time Left: %ld s, Fan Speed: %d%%", 
    //          current_config.state, current_config.temperature, current_config.time_s, current_config.SPEED);
    return current_config.state;
}
