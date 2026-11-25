# The ESP-IDF application links the public MicroMeerschaum component.
add_library(usermod_mm INTERFACE)
target_sources(usermod_mm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/native/modmm.c)
target_include_directories(usermod_mm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../../core/include)
target_link_libraries(usermod INTERFACE usermod_mm)
if(CONFIG_MICROMEERSCHAUM_SQLITE)
    include(${CMAKE_CURRENT_LIST_DIR}/sqlite/micropython.cmake)
endif()
