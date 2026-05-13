idf_component_register(SRCS "WIFI.c" 
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES common esp_wifi wpa_supplicant qrcode common)