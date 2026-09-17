# Sherlock — tools/Sherlock/CMakeLists.txt
# The Sherlock project: its version, its modules and their edges, its tests.
cmake_minimum_required(VERSION 3.28)
project(Sherlock VERSION 0.1.0 LANGUAGES C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/SherlockDependencies.cmake)

function(sherlock_compile_options target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /std:c++latest /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    target_compile_definitions(${target} PRIVATE SHERLOCK_VERSION="${PROJECT_VERSION}")
endfunction()

# sherlock_module(<Name> SOURCES <files…> LINKS <targets…>)
function(sherlock_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINKS" ${ARGN})
    add_library(Sherlock${name} STATIC ${ARG_SOURCES})
    add_library(Sherlock::${name} ALIAS Sherlock${name})
    target_include_directories(Sherlock${name} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/Source/${name}/include)
    target_link_libraries(Sherlock${name} PUBLIC ${ARG_LINKS})
    sherlock_compile_options(Sherlock${name})
endfunction()

set(S ${CMAKE_CURRENT_LIST_DIR}/Source)

sherlock_module(Foundation
    SOURCES ${S}/Foundation/Diagnostic.cpp ${S}/Foundation/MappedFile.cpp)

option(SHERLOCK_BUILD_TESTS "Builds Sherlock's gates" ON)
if(SHERLOCK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/Sherlock ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
