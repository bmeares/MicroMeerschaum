# ESP-IDF names a local component after its checkout directory.
get_filename_component(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(core_component "${core_dir}" NAME)
idf_component_register(SRCS "main.cpp" PRIV_REQUIRES log "${core_component}")
