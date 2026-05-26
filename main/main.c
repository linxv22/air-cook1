#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "WIFI.h"
#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
#include "app_events.h"
#include "my_audio.h"
#include "ui_con.h"

static const char *TAG = "main";

void app_event_init (void);



// WiFI，UI界面
void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    LCD_Init();
    app_event_init();
    ui_start();
    v220_con_init();
    ntc_adc_init();
    wifi_init();
    my_audio_init();
    
    //修改测试代码

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // 打印所有可用内存（内部 DRAM + 外部 PSRAM）
        uint32_t free_heap = esp_get_free_heap_size();
        // 专门打印内部 DRAM 的可用内存
        uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // 专门打印外部 PSRAM 的可用内存 (如果有的话)
        uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        

        ESP_LOGI(TAG, "Main loop running... Free Heap: %lu Bytes, Internal RAM: %lu Bytes, PSRAM: %lu Bytes", 
                 free_heap, free_internal, free_psram);
    }
}
