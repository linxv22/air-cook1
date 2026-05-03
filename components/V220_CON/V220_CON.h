#ifndef V220_CON_H
#define V220_CON_H

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" 
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_intr_types.h"


// Function prototypes for V220_CON component
void v220_con_init(void);
// Function to test the V220_CON component (e.g., turn on the fan for a short period)
void v220_con_test(void);
void V220_HOT_CON(bool con);
void V220_FAN_CON(bool con ,uint32_t speed);
// Your header file content here

#endif // V220_CON_H