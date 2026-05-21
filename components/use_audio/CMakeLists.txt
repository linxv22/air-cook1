idf_component_register(SRCS "my_audio.c" 
                    INCLUDE_DIRS "." 
                    REQUIRES 
                    audio_pipeline audio_sal audio_hal esp_peripherals audio_board audio_stream
                    common 
                    EMBED_FILES 
                    "music-16b-2c-8000hz.mp3" 
                    "music-16b-2c-22050hz.mp3" 
                    "music-16b-2c-44100hz.mp3"
                    )
