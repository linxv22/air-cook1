#include "V220_CON.h"




#define TAG "V220_CON"

#define HERT_CON_GPIO GPIO_NUM_5


#define FAN_CON_GPIO GPIO_NUM_4
#define ZERO_CROSS GPIO_NUM_3


// ================= 全局控制变量 =================
volatile static bool is_fan_on = false;            // 启停开关
volatile static uint32_t delay_time_us = 1000;     // 延时时间(控制转速)，范围 0-9900

esp_timer_handle_t triac_timer;
volatile static bool is_second_half = false;       // 标记是否为下半周期触发

// ================= ISR 和 定时器回调 (在内部RAM中执行) =================

// 1. 定时器回调：打出触发脉冲，并接力下半波
static void IRAM_ATTR triac_timer_callback(void* arg)
{
    // A. 触发动作：拉高 50us 后拉低
    gpio_set_level(FAN_CON_GPIO, 1);
    esp_rom_delay_us(200);
    gpio_set_level(FAN_CON_GPIO, 0);

    // B. 接力逻辑：如果当前是上半波，立刻定一个 10ms 后的闹钟触发下半波
    if (!is_second_half) {
        is_second_half = true;
        esp_timer_start_once(triac_timer, 10000-200); // 10ms 减去触发脉冲的时间
    }
}

// 2. 过零下降沿中断：每 20ms 触发一次
static void IRAM_ATTR zcd_isr_handler(void* arg)
{
    // 如果风扇开关没开，直接无视过零信号
    if (!is_fan_on) {
        gpio_set_level(FAN_CON_GPIO, 0);
        return; 
    }

    // 重置标记，准备触发上半波
    is_second_half = false;
    
    // 启动延时，时间到了就去执行 triac_timer_callback
    esp_timer_start_once(triac_timer, delay_time_us);
}

// ================= 初始化任务 =================
// 专门在 Core 1 上运行，把中断绑定到 Core 1
void triac_init_core1_task(void *arg)
{
    ESP_LOGI(TAG, "正在 Core %d 初始化硬件...", xPortGetCoreID());


    // 2. 配置高精度定时器 (ISR模式)
    esp_timer_create_args_t timer_args = {
        .callback = &triac_timer_callback,
        .arg = NULL,
        .name = "triac_timer",
        .dispatch_method = ESP_TIMER_ISR 
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &triac_timer));

    // 3. 配置 ZCD 输入脚 (下降沿触发)
    gpio_config_t zcd_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ZERO_CROSS),
        .pull_up_en = 1 // 启用内部上拉，防止引脚悬空乱跳
    };
    gpio_config(&zcd_conf);

    // 4. 注册并挂载中断
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ZERO_CROSS, zcd_isr_handler, NULL);

    ESP_LOGI(TAG, "初始化完成，中断已就绪！");
    vTaskDelete(NULL); // 初始化完就销毁任务
}

void v220_con_init(void) {
    
    gpio_config_t io_conf = {
    .intr_type = GPIO_INTR_DISABLE, // Disable interrupts
    .mode = GPIO_MODE_OUTPUT,       // Set as output mode
    .pin_bit_mask = (1ULL << HERT_CON_GPIO) | (1ULL << FAN_CON_GPIO), // Bit mask for the pins
    .pull_down_en = GPIO_PULLDOWN_ENABLE, // Disable pull-down
    .pull_up_en = GPIO_PULLUP_DISABLE    // Disable pull-up
    };
    gpio_config(&io_conf); // Configure the GPIO with the specified settings

    gpio_set_level(HERT_CON_GPIO, 0); // Set the initial state of the heater control pin to LOW (off)
    gpio_set_level(FAN_CON_GPIO, 0);  // Set the initial state of the fan control pin to LOW (off)

    // .把初始化任务扔到 Core 1 运行 (防止Wi-Fi在Core 0干扰)
    xTaskCreatePinnedToCore(triac_init_core1_task, "init_task", 2048, NULL, configMAX_PRIORITIES-1, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(500)); // 等待初始化完毕



    ESP_LOGI(TAG, "V220_CON initialized");

}

void v220_con_test(void) {
    // gpio_set_level(FAN_CON_GPIO, 1); 
    // ESP_LOGI(TAG, "Fan turned ON");
    // vTaskDelay(pdMS_TO_TICKS(2000)); 
    // gpio_set_level(HERT_CON_GPIO, 1);
    // ESP_LOGI(TAG, "Heater turned ON");
    // vTaskDelay(pdMS_TO_TICKS(10000)); // Fan on for 10 seconds
    // gpio_set_level(FAN_CON_GPIO, 0);
    // gpio_set_level(HERT_CON_GPIO, 0);
    // ESP_LOGI(TAG, "Fan and heater turned OFF");

     // [状态 1] 停止运行
        ESP_LOGI(TAG, "状态: 风扇关闭");
        is_fan_on = false;
        gpio_set_level(HERT_CON_GPIO, 0); // 确保加热器也关了
        vTaskDelay(pdMS_TO_TICKS(5000)); // 保持 5 秒

        // [状态 2] 满速运行 (导通延时极小)
        ESP_LOGI(TAG, "状态: 满速运行 (延时 1000us),5s");
        delay_time_us = 1000;
        is_fan_on = true;
        vTaskDelay(pdMS_TO_TICKS(5000)); // 保持 5 秒
        // [状态 3] 中速运行 (导通延时中等)

        vTaskDelay(pdMS_TO_TICKS(5000)); // 保持 5 秒
        delay_time_us = 5000;
        is_fan_on = true;

        vTaskDelay(pdMS_TO_TICKS(5000)); // 保持 5 秒
        // [状态 4] 慢速运行 (导通延时较大)
        ESP_LOGI(TAG, "状态: 慢速运行 (延时 8500us)");
        delay_time_us = 8500;
     
        vTaskDelay(pdMS_TO_TICKS(10000)); // 保持 10 秒
        gpio_set_level(FAN_CON_GPIO, 0);
        gpio_set_level(HERT_CON_GPIO, 0);

    
}

//电热丝控制函数
//参数:con: true-开，false-关
void V220_HOT_CON(bool con)
{
    if(con == true)
    {
        gpio_set_level(HERT_CON_GPIO, 1);
    }
    else
    {
        gpio_set_level(HERT_CON_GPIO, 0);
    }

}

//风扇控制函数
//参数:con: true-开，false-关 speed：0-100，表示风扇转速百分比
void V220_FAN_CON(bool con ,uint32_t speed)
{
    if(con == true)
    {
        is_fan_on = true;
        // 根据 speed 计算 delay_time_us，假设 speed=0 对应 9900us，speed=100 对应 1000us
        if (speed > 100) speed = 100; // 限制 speed 在 0-100 范围内
        delay_time_us = 1000 + (100 - speed) * 90; // 线性映射
    }
    else
    {
        gpio_set_level(FAN_CON_GPIO, 0);
        is_fan_on = false;
    }
}
