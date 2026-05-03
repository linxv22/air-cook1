idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES V220_CON NTC_ADC SPI_LCD_TOUCH esp_event )
