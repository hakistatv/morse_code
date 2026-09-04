idf_component_register(SRCS "morse_code.c"
                       PRIV_REQUIRES epd_1in54 text_display morse_player boot_button
                                     message_store device_settings audio_settings
                                     wifi_ap mdns_service web_server
                       INCLUDE_DIRS "")
