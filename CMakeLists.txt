# ESP-IDF component entry point. Host builds use core/CMakeLists.txt directly.
if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "Build the host core with: cmake -S core -B build")
endif()

idf_component_register(
    SRCS "core/src/crc32.c" "core/src/wire.c" "core/src/record.c" "core/src/segment.c" "core/src/batch.c"
         "core/src/protocol.c" "core/src/document.c" "core/src/pipe.c" "core/src/gatt.c"
         "core/storage/journal.c" "core/storage/volume.c" "core/posix/peer.c" "core/shared/sender.c"
    INCLUDE_DIRS "core/include"
    PRIV_REQUIRES esp_http_client mbedtls esp_hw_support esp_rom pthread
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

if(CONFIG_MICROMEERSCHAUM_HTTPS)
    target_sources(${COMPONENT_LIB} PRIVATE "ports/esp-idf/https.c")
endif()

if(CONFIG_MICROMEERSCHAUM_SQLITE)
    include(core/sqlite/engine.cmake)
    target_sources(${COMPONENT_LIB} PRIVATE "core/sqlite/instance.c" "core/sqlite/write.c"
        "core/sqlite/rows.c" "core/sqlite/outbox.c")
    target_link_libraries(${COMPONENT_LIB} PUBLIC mm_sqlite_engine)
endif()
