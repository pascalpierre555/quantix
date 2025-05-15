idf_component_register(SRCS "wifi_manager.c" "dns_server.c" "http_app.c" "json.c" "nvs_sync.c"
                    INCLUDE_DIRS "include"
                    REQUIRES log nvs_flash mdns wpa_supplicant lwip esp_http_server esp_wifi
                    EMBED_FILES src/style.css src/code.js src/index.html)
