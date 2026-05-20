idf_component_register(SRCS "./custom_board/board.c"  "./custom_board/board_pins_config.c"
                    INCLUDE_DIRS "./custom_board" "./include"
                    PRIV_REQUIRES common  esp_peripherals audio_sal audio_hal esp_dispatcher  )
