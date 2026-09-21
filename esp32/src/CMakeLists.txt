# crc32.c and sevenbit.c are the keyboard firmware's own sources, compiled
# in here rather than reimplemented, so the two ends of the firmware
# transfer cannot disagree about the checksum or the 7-in-8 packing. The
# same directory supplies otaproto.h and otamap.h, which define the wire
# format and the flash layout the transfer targets.
idf_component_register(
    SRCS "main.c" "sysex_bridge.c" "editor.c" "captive_dns.c"
         "firmware_update.c"
         "../../stm32/src/crc32.c" "../../stm32/src/sevenbit.c"
    INCLUDE_DIRS "." "../../stm32/include"
    EMBED_FILES "editor.html"
    REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio esp_timer
             esp_wifi esp_netif esp_event esp_http_server lwip
             app_update esp_app_format
)
