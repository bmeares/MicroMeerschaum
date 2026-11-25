set(IDF_TARGET esp32s3)
# Reject stock v1.28.0 and the earlier passkey-only patch.
set(mm_nimble_source "${CMAKE_CURRENT_SOURCE_DIR}/../../extmod/nimble/modbluetooth_nimble.c")
if(EXISTS "${mm_nimble_source}")
    file(SHA256 "${mm_nimble_source}" mm_nimble_sha)
    if(mm_nimble_sha STREQUAL "6011b234f0737c6b4ecdf5384d0928729b5b7cc85a6746168e545c96ae40ad84"
       OR mm_nimble_sha STREQUAL "e7ea4f371fdb50f563cb9319ab8bf3c5ee058a0d84b9a3af79efb79ebe1113fa")
        message(FATAL_ERROR "Apply ports/micropython/patches/v1.28.0-nimble-passkey.patch to MicroPython before building this board.")
    endif()
endif()
set(mm_board "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(mm_component "${mm_board}/../../../.." ABSOLUTE)
list(APPEND EXTRA_COMPONENT_DIRS "${mm_component}" "${mm_board}/mm_filesystem")
configure_file("${mm_board}/sdkconfig.board.in" "${CMAKE_BINARY_DIR}/sdkconfig.mm" @ONLY)
set(SDKCONFIG_DEFAULTS boards/sdkconfig.base boards/sdkconfig.ble boards/sdkconfig.spiram_sx
    "${CMAKE_BINARY_DIR}/sdkconfig.mm")
set(MICROPY_FROZEN_MANIFEST "${mm_board}/manifest.py")
set(USER_C_MODULES "${mm_board}/usermods.cmake")
