add_library(usermod_mmstorage INTERFACE)
target_include_directories(usermod_mmstorage INTERFACE ${CMAKE_CURRENT_LIST_DIR}/mm_filesystem/include)
target_sources(usermod_mmstorage INTERFACE ${CMAKE_CURRENT_LIST_DIR}/mount.c ${CMAKE_CURRENT_LIST_DIR}/https.c)
get_filename_component(mm_component "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(mm_component_name "${mm_component}" NAME)
# Keep IDF's conditional include paths out of MicroPython's QSTR source walk.
target_link_libraries(usermod_mmstorage INTERFACE
    "$<BUILD_INTERFACE:idf::mm_filesystem>" "$<BUILD_INTERFACE:idf::${mm_component_name}>")
target_link_libraries(usermod INTERFACE usermod_mmstorage)
include("${CMAKE_CURRENT_LIST_DIR}/../../micropython.cmake")
