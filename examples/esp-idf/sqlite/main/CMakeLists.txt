get_filename_component(mm_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(mm_component "${mm_root}" NAME)
idf_component_register(SRCS "sqlite_example.c" INCLUDE_DIRS "." PRIV_REQUIRES ${mm_component} joltwallet__littlefs)
