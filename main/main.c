#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "NTC_ADC.h"
#include "V220_CON.h"
#include "LCD.h"
#include "app_events.h"

static const char *TAG = "main";
    
void app_event_init (void);
void ui_staret(void);

void app_main(void)
{
    
    LCD_Init();
    app_event_init();
    ui_staret();
    v220_con_init();
    ntc_adc_init();
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}

