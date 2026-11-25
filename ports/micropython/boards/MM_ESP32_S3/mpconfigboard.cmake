set(IDF_TARGET esp32s3)
set(mm_board "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(mm_component "${mm_board}/../../../.." ABSOLUTE)
list(APPEND EXTRA_COMPONENT_DIRS "${mm_component}")
configure_file("${mm_board}/sdkconfig.board.in" "${CMAKE_BINARY_DIR}/sdkconfig.mm" @ONLY)
set(SDKCONFIG_DEFAULTS boards/sdkconfig.base boards/sdkconfig.ble boards/sdkconfig.spiram_sx
    "${CMAKE_BINARY_DIR}/sdkconfig.mm")
set(MICROPY_FROZEN_MANIFEST "${mm_board}/manifest.py")
set(USER_C_MODULES "${mm_board}/usermods.cmake")
