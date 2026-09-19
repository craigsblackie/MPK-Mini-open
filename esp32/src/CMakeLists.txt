idf_component_register(
    SRCS "main.c" "sysex_bridge.c" "editor.c" "captive_dns.c"
    INCLUDE_DIRS "."
    EMBED_FILES "editor.html"
    REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio esp_timer
             esp_wifi esp_netif esp_event esp_http_server lwip
)
