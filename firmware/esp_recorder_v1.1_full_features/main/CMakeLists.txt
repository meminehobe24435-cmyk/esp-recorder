idf_component_register(SRCS "app_main.c" "console_cmd.c" "sd_speed_test.c" "web_server.c" "config_manager.c" "data_recorder.c" "data_router.c" "wifi_config.c" "usb_cdc.c" "usb_msc.c" "uart_driver.c" "sd_card.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "index.html")
