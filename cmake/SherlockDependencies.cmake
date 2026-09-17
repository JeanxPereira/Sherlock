# Sherlock — tools/Sherlock/cmake/SherlockDependencies.cmake
# The three third-party libraries, pinned by tag or by hash.
include(FetchContent)

FetchContent_Declare(sqlite
    URL      https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d)
FetchContent_MakeAvailable(sqlite)
add_library(SherlockSqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(SherlockSqlite PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(SherlockSqlite PUBLIC
    SQLITE_ENABLE_FTS5 SQLITE_DQS=0 SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
    target_compile_options(SherlockSqlite PRIVATE /W0)
endif()

set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARM64_SUPPORT        ON  CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL         OFF CACHE BOOL "" FORCE)
set(CAPSTONE_INSTALL              OFF CACHE BOOL "" FORCE)
FetchContent_Declare(capstone
    GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
    GIT_TAG        5.0.9
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(capstone)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)
