get_filename_component(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(core_component "${core_dir}" NAME)
idf_component_register(SRCS "session.c" PRIV_REQUIRES "${core_component}" json bt nvs_flash
                       esp_timer esp_driver_uart esp_driver_usb_serial_jtag)
