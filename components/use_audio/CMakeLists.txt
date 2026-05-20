idf_component_register(SRCS "my_audio.c" 
                    INCLUDE_DIRS "." 
                    REQUIRES 
                    audio_pipeline audio_sal audio_hal esp_peripherals audio_board audio_stream
                    common )
