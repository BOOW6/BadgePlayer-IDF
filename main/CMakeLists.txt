idf_component_register(SRCS "main.c" "audio_player.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp-idf-pn532
                            esp_audio_simple_player
                            esp_wifi
                            esp_event
                            esp_netif
                            nvs_flash)
