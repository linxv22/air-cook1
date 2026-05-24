idf_component_register(SRCS "my_audio.c" 
                    INCLUDE_DIRS "." 
                    REQUIRES 
                    esp_actions audio_sal audio_stream audio_hal audio_board audio_recorder esp_peripherals 
                    common 
                   # EMBED_FILES 
                   # "music-16b-2c-8000hz.mp3" 
                   # "music-16b-2c-22050hz.mp3" 
                   # "music-16b-2c-44100hz.mp3"
                    )
