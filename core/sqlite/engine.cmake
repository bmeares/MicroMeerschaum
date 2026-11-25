# Pinned upstream SQLite amalgamation. Set FETCHCONTENT_SOURCE_DIR_MM_SQLITE3
# to an extracted copy of this archive for offline builds.
include(FetchContent)
FetchContent_Declare(mm_sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip
    URL_HASH SHA256=acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4)
FetchContent_MakeAvailable(mm_sqlite3)
set(mm_amalgamation "${mm_sqlite3_SOURCE_DIR}/sqlite3.c")
if(ESP_PLATFORM)
    # SQLite selects the pthread mutex implementation with the Unix VFS.
    # IDF provides pthreads with our own OS_OTHER VFS. Select those two
    # independent capabilities without importing the Unix filesystem code.
    file(READ "${mm_amalgamation}" mm_source)
    string(REPLACE "#  if SQLITE_OS_UNIX\n#    define SQLITE_MUTEX_PTHREADS"
        "#  if SQLITE_OS_UNIX || defined(ESP_PLATFORM)\n#    define SQLITE_MUTEX_PTHREADS"
        mm_source "${mm_source}")
    set(mm_amalgamation "${CMAKE_CURRENT_BINARY_DIR}/sqlite3-esp.c")
    file(WRITE "${mm_amalgamation}" "${mm_source}")
    unset(mm_source)
endif()
add_library(mm_sqlite_engine STATIC "${mm_amalgamation}")
target_include_directories(mm_sqlite_engine PUBLIC "${mm_sqlite3_SOURCE_DIR}")
target_compile_definitions(mm_sqlite_engine PRIVATE SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_WAL SQLITE_TEMP_STORE=3 SQLITE_DQS=0 SQLITE_DEFAULT_PAGE_SIZE=1024
    SQLITE_DEFAULT_CACHE_SIZE=-32 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_DEPRECATED)
if(ESP_PLATFORM)
    target_compile_definitions(mm_sqlite_engine PRIVATE SQLITE_OS_OTHER=1 SQLITE_THREADSAFE=1 NDEBUG=1
        SQLITE_OMIT_LOCALTIME SQLITE_MAX_MMAP_SIZE=0)
    target_compile_options(mm_sqlite_engine PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp-idf/sqlite_release.h" -Os)
    target_sources(mm_sqlite_engine PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp-idf/sqlite_vfs.c")
    target_link_libraries(mm_sqlite_engine PRIVATE idf::newlib idf::pthread idf::esp_hw_support idf::esp_rom)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mm_sqlite_engine PUBLIC Threads::Threads m)
endif()
