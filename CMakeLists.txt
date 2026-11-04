# ESP-IDF component entry point. Host builds use core/CMakeLists.txt directly.
if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "Build the host core with: cmake -S core -B build")
endif()

idf_component_register(
    SRCS "core/src/crc32.c" "core/src/wire.c" "core/src/record.c" "core/src/segment.c" "core/src/batch.c"
    INCLUDE_DIRS "core/include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
