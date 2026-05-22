idf_component_register(SRCS "app_task.c" "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES V220_CON NTC_ADC SPI_LCD_TOUCH use_audio esp_event UI_CON common nvs_flash WIFI)
