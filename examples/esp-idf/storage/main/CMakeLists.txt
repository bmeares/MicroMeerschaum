get_filename_component(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(core_component "${core_dir}" NAME)
idf_component_register(SRCS "storage.c" PRIV_REQUIRES "${core_component}" fatfs sdmmc esp_driver_sdmmc joltwallet__littlefs)
