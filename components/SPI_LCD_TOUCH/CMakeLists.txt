idf_component_register(SRCS "LCD.c" 
                    INCLUDE_DIRS "." 
                    REQUIRES 
                    esp_lcd esp_driver_gpio esp_hal_lcd lvgl esp_timer esp_lcd_touch_xpt2046  
                    common )
