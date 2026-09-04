idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server
                       PRIV_REQUIRES message_store text_display epd_1in54 device_settings
                                     audio_settings morse_player
                       EMBED_TXTFILES "pages/home.html" "pages/messages.html" "pages/audio.html"
                                      "pages/settings.html" "pages/restart.html" "pages/style.css")
