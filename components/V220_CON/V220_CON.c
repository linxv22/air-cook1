#include "V220_CON.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define TAG "V220_CON"

#define HERT_CON_GPIO GPIO_NUM_5
#define FAN_CON_GPIO GPIO_NUM_4
#define ZERO_CROSS GPIO_NUM_3

// ================= 全局控制变量 =================
volatile static bool is_fan_on = false;            
volatile static uint32_t delay_time_us = 1000;     // 控制转速，范围 0-9900

static rmt_channel_handle_t rmt_tx_chan = NULL;
static rmt_encoder_handle_t rmt_copy_encoder = NULL;
static TaskHandle_t rmt_tx_task_handle = NULL;

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
    ESP_LOGI(TAG, "V220_CON RMT initialized successful!");
}

// ================= 测试与接口函数 =================
void v220_con_test(void) {
    // 原始状态流程
    ESP_LOGI(TAG, "状态: 风扇关闭");
    is_fan_on = false;
    gpio_set_level(HERT_CON_GPIO, 0); // 确保加热器关闭
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    ESP_LOGI(TAG, "状态: 满速运行 (延时 1000us)");
    delay_time_us = 1000;
    is_fan_on = true;
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    ESP_LOGI(TAG, "状态: 中速运行 (延时 5000us)");
    delay_time_us = 5000;
    is_fan_on = true;
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    ESP_LOGI(TAG, "状态: 慢速运行 (延时 8500us)");
    delay_time_us = 8500;
    is_fan_on = true;
    vTaskDelay(pdMS_TO_TICKS(10000)); 

    // 结束测试
    is_fan_on = false;
    gpio_set_level(HERT_CON_GPIO, 0);
}

// 电热丝控制函数 
// 参数: con: true-开，false-关
void V220_HOT_CON(bool con)
{
    if(con == true) {
        gpio_set_level(HERT_CON_GPIO, 1);
    } else {
        gpio_set_level(HERT_CON_GPIO, 0);
    }
}

// 风扇控制函数
// 参数: con: true-开，false-关 speed：0-100，表示风扇转速百分比
void V220_FAN_CON(bool con, uint32_t speed)
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
