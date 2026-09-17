# Sherlock — tests/Sherlock/CMakeLists.txt
# Sherlock's gates; the corpus paths come from target.py at configure time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHERLOCK_REPO ${CMAKE_CURRENT_LIST_DIR}/../..)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${SHERLOCK_REPO}/References/scripts/target.py --paths
    OUTPUT_VARIABLE SHERLOCK_TARGET_PATHS OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "corpus=([^\n]*)" _ "${SHERLOCK_TARGET_PATHS}")
set(SHERLOCK_CORPUS "${CMAKE_MATCH_1}")
string(REGEX MATCH "target=([^\n]*)" _ "${SHERLOCK_TARGET_PATHS}")
set(SHERLOCK_BUILD "${CMAKE_MATCH_1}")
set(SHERLOCK_CACHE_DIR "${SHERLOCK_CORPUS}/${SHERLOCK_BUILD}/dsc/${SHERLOCK_BUILD}__MacOS")
set(SHERLOCK_STORE_DIR "${SHERLOCK_CORPUS}/${SHERLOCK_BUILD}/Sherlock")
set(SHERLOCK_TEST_ENV "SHERLOCK_CACHE=${SHERLOCK_CACHE_DIR};SHERLOCK_STORE=${SHERLOCK_STORE_DIR}")

# sherlock_gate(<File> LINKS <targets…>) -> executable SherlockTest<File>, test Sherlock.<File>
function(sherlock_gate file)
    cmake_parse_arguments(ARG "" "" "LINKS" ${ARGN})
    add_executable(SherlockTest${file} ${CMAKE_CURRENT_LIST_DIR}/${file}.cpp)
    target_include_directories(SherlockTest${file} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(SherlockTest${file} PRIVATE ${ARG_LINKS})
    sherlock_compile_options(SherlockTest${file})
    add_test(NAME Sherlock.${file} COMMAND SherlockTest${file})
    set_tests_properties(Sherlock.${file} PROPERTIES ENVIRONMENT "${SHERLOCK_TEST_ENV}")
endfunction()

sherlock_gate(FoundationGates LINKS Sherlock::Foundation)
sherlock_gate(StoreGates LINKS Sherlock::Store)
sherlock_gate(DyldSharedCacheGates LINKS Sherlock::DyldSharedCache)
sherlock_gate(MachOGates LINKS Sherlock::DyldSharedCache)
