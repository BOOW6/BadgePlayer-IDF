idf_component_register(SRCS "main.c"
                            "audio_player.c"
                            "badge_cfg.c"
                            "badge_cloud.c"
                            "ble_prov.c"
                            "voice.c"
                            "voice_data.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver
                            esp-idf-pn532
                            esp_audio_simple_player
                            esp_wifi
                            esp_event
                            esp_netif
                            nvs_flash
                            bt
                            esp_http_client
                            esp-tls
                            mbedtls
                            json)
