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

static const char *TAG = "main";
    
void app_event_init (void);
void ui_staret(void);

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
    ui_staret();
    v220_con_init();
    ntc_adc_init();
    // wifi_init();



    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}

