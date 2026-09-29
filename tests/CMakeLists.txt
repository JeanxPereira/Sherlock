# Sherlock — tests/CMakeLists.txt
# Sherlock's gates: fixture gates always; corpus gates when SHERLOCK_CACHE and SHERLOCK_STORE name
# a corpus at configure time; layer-2 gates when an IDA matching its SDK was found as well.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SHERLOCK_CACHE  "" CACHE PATH "Extracted dyld shared cache (<build>__MacOS) the corpus gates read")
set(SHERLOCK_STORE  "" CACHE PATH "Store directory (Catalog.db, Images/) the corpus gates read and fill")
set(SHERLOCK_DYLIBS "" CACHE PATH "Extracted dylibs directory the layer-2 export gates decompile")

set(S ${PROJECT_SOURCE_DIR}/Source)
set(SHERLOCK_FIXTURES ${CMAKE_CURRENT_LIST_DIR}/fixtures)

set(SHERLOCK_CORPUS OFF)
if(SHERLOCK_CACHE AND SHERLOCK_STORE AND IS_DIRECTORY "${SHERLOCK_CACHE}")
    set(SHERLOCK_CORPUS ON)
    get_filename_component(_sherlock_cache_leaf "${SHERLOCK_CACHE}" NAME)
    string(REGEX REPLACE "__MacOS$" "" SHERLOCK_BUILD "${_sherlock_cache_leaf}")
    message(STATUS "Sherlock: corpus gates on -- build ${SHERLOCK_BUILD} at ${SHERLOCK_CACHE}")
else()
    message(STATUS "Sherlock: corpus gates off -- SHERLOCK_CACHE / SHERLOCK_STORE name no corpus")
endif()

set(SHERLOCK_TEST_ENV
    "SHERLOCK_CACHE=${SHERLOCK_CACHE};SHERLOCK_STORE=${SHERLOCK_STORE};SHERLOCK_TOWERS=${SHERLOCK_FIXTURES}/consumer/towers.json")

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

# ---- Fixture gates: no corpus, every machine.
sherlock_gate(FoundationGates LINKS Sherlock::Foundation)
sherlock_gate(StoreGates LINKS Sherlock::Store)
sherlock_gate(DocumentIndexGates LINKS Sherlock::DocumentIndex)
sherlock_gate(ConfigurationGates LINKS Sherlock::Configuration)
sherlock_gate(HexRaysStoreGates LINKS Sherlock::HexRaysExport)

sherlock_gate(CliGates LINKS Sherlock::Facts Sherlock::HexRaysExport)
target_sources(SherlockTestCliGates PRIVATE
    ${S}/SherlockCli/Arguments.cpp ${S}/SherlockCli/Towers.cpp ${S}/SherlockCli/Demangler.cpp)
target_include_directories(SherlockTestCliGates PRIVATE ${S}/SherlockCli/include)
target_link_libraries(SherlockTestCliGates PRIVATE nlohmann_json::nlohmann_json)

sherlock_gate(DocumentsGates LINKS Sherlock::Store Sherlock::DocumentIndex Sherlock::Configuration Sherlock::DyldSharedCache)
target_sources(SherlockTestDocumentsGates PRIVATE ${S}/SherlockCli/Documents.cpp)
target_include_directories(SherlockTestDocumentsGates PRIVATE ${S}/SherlockCli/include)

# Queries.cpp reads layer 2 and disassembles for --asm, so this target carries both modules.
sherlock_gate(DocumentLayerGates LINKS Sherlock::Store Sherlock::DocumentIndex Sherlock::Configuration Sherlock::DyldSharedCache Sherlock::Facts Sherlock::HexRaysExport)
target_sources(SherlockTestDocumentLayerGates PRIVATE ${S}/SherlockCli/Queries.cpp)
target_include_directories(SherlockTestDocumentLayerGates PRIVATE ${S}/SherlockCli/include)

add_executable(SherlockDocumentsFixture ${CMAKE_CURRENT_LIST_DIR}/DocumentsFixture.cpp)
target_link_libraries(SherlockDocumentsFixture PRIVATE Sherlock::Store)
sherlock_compile_options(SherlockDocumentsFixture)

add_test(NAME Sherlock.LaudoExactOutputGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/LaudoExactOutputGate.py
                $<TARGET_FILE:Sherlock> $<TARGET_FILE:SherlockDocumentsFixture>
                ${CMAKE_CURRENT_BINARY_DIR}/LaudoExactOutputScratch)

# laudo's own staleness gate: a scratch consumer, built and mutated with the real Sherlock.exe.
add_test(NAME Sherlock.LaudoStalenessGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/LaudoStalenessGate.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR}/LaudoStalenessScratch)

foreach(scenario IN ITEMS meta-prepare meta-step images)
    add_test(NAME Sherlock.QueryHeaderFailure.${scenario}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/QueryHeaderFailure.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR} ${scenario})
endforeach()

add_test(NAME Sherlock.FinalFixGates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/FinalFixGates.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR}/FinalFixGateScratch)

# The parent of `build hexrays` driven with a stand-in worker: the resume reset, a failed image
# and the free-space floor are gated on every machine, with or without IDA.
add_test(NAME Sherlock.HexRaysBuildGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysBuildGate.py)
set_tests_properties(Sherlock.HexRaysBuildGate PROPERTIES
        ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>" TIMEOUT 600)

# ---- IDA gates. The worker target exists only where an SDK matching the installed decompiler was
# found, and a gate over a target that was never built would report IDA's absence as a defect.
if(SHERLOCK_IDA_FOUND)
    add_test(NAME Sherlock.HexRaysSdkGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysSdkGate.py)
    set_tests_properties(Sherlock.HexRaysSdkGate PROPERTIES ENVIRONMENT
            "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_IDA_API=${SHERLOCK_IDA_API}")

    # The mutation of the pairing check, kept as a gate: script mode exercises the module alone.
    add_test(NAME Sherlock.HexRaysSdkRefusal
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysSdkRefusalGate.py)
    set_tests_properties(Sherlock.HexRaysSdkRefusal PROPERTIES ENVIRONMENT
            "CMAKE_COMMAND=${CMAKE_COMMAND};SHERLOCK_IDA_SDK_MODULE=${PROJECT_SOURCE_DIR}/cmake/SherlockIdaSdk.cmake;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_IDA_API=${SHERLOCK_IDA_API}")

    add_test(NAME Sherlock.HexRaysLeftover
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysLeftoverGate.py)
    set_tests_properties(Sherlock.HexRaysLeftover PROPERTIES
            ENVIRONMENT "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR}"
            TIMEOUT 600)
endif()

# ---- Corpus gates.
if(SHERLOCK_CORPUS)
    sherlock_gate(DyldSharedCacheGates LINKS Sherlock::DyldSharedCache)
    sherlock_gate(MachOGates LINKS Sherlock::DyldSharedCache)
    sherlock_gate(FactsGates LINKS Sherlock::Facts)
    sherlock_gate(VirtualCallGates LINKS Sherlock::Facts Sherlock::Store)

    function(sherlock_query_test name)
        cmake_parse_arguments(ARG "" "MATCH" "" ${ARGN})
        add_test(NAME Sherlock.${name} COMMAND $<TARGET_FILE:Sherlock> ${ARG_UNPARSED_ARGUMENTS}
                --cache ${SHERLOCK_CACHE} --store ${SHERLOCK_STORE})
        set_tests_properties(Sherlock.${name} PROPERTIES
            FIXTURES_REQUIRED SherlockStore
            PASS_REGULAR_EXPRESSION "${ARG_MATCH}")
    endfunction()

    sherlock_query_test(QueryOwner    q 0x240622d98                          MATCH "DesignLibrary")
    sherlock_query_test(QueryHeader   q 0x240622d98                          MATCH "build 26A5416b")
    sherlock_query_test(QueryOwnerCoverage q 0x240622d98                     MATCH "verdict: FOUND 1  coverage 23102602/23103016")
    sherlock_query_test(QueryCallerSummary q 0x2230edebc                     MATCH "callers: 5")
    sherlock_query_test(QueryUnmapped q 0x7ffff0000                          MATCH "verdict: NOT VERIFIED")
    sherlock_query_test(QueryCallers  callers 0x2230edebc                    MATCH "0x240679e58")
    sherlock_query_test(QueryCalls    calls 0x2406780f0                      MATCH "verdict: FOUND")
    sherlock_query_test(QueryNotAFunction calls 0x2406a9db0                  MATCH "NOT VERIFIED")
    sherlock_query_test(QueryRefs     refs 0x29f60f388 --to 0x29f60f480      MATCH "verdict: FOUND 24")

    add_test(NAME Sherlock.QueryIncomplete
            COMMAND ${Python3_EXECUTABLE} -c
            "import subprocess,sys; p=subprocess.run(sys.argv[1:],capture_output=True,text=True); print(p.stdout,end=''); sys.exit(0 if p.returncode == 3 and 'verdict: PARTIAL coverage incomplete' in p.stdout else 1)"
            $<TARGET_FILE:Sherlock> calls 0x240492428
            --cache ${SHERLOCK_CACHE} --store ${SHERLOCK_STORE})
    set_tests_properties(Sherlock.QueryIncomplete PROPERTIES FIXTURES_REQUIRED SherlockStore)

    add_test(NAME Sherlock.QueryCallersWithoutCache
            COMMAND $<TARGET_FILE:Sherlock> callers 0x2230edebc --store ${SHERLOCK_STORE})
    set_tests_properties(Sherlock.QueryCallersWithoutCache PROPERTIES
            FIXTURES_REQUIRED SherlockStore
            PASS_REGULAR_EXPRESSION "0x240679e58")

    # A consumer with one evidence page, so the configuration loads; --documents names a path that
    # never exists, so the answer cannot depend on run order.
    set(SHERLOCK_NO_DOCUMENTS_ROOT ${CMAKE_CURRENT_BINARY_DIR}/SherlockNoDocumentsConsumer)
    file(MAKE_DIRECTORY ${SHERLOCK_NO_DOCUMENTS_ROOT}/docs/re)
    file(WRITE ${SHERLOCK_NO_DOCUMENTS_ROOT}/docs/re/page.md "## 1 A page

Text.
")
    file(WRITE ${SHERLOCK_NO_DOCUMENTS_ROOT}/sherlock.json
        "{ \"schema\": 1, \"sherlock\": \"0.2\", \"collections\": [ { \"path\": \"docs/re\", \"kind\": \"evidence\" } ] }
")
    add_test(NAME Sherlock.QueryNoDocuments
            COMMAND $<TARGET_FILE:Sherlock> q 0x240622d98 --cache ${SHERLOCK_CACHE} --store ${SHERLOCK_STORE}
                    --config ${SHERLOCK_NO_DOCUMENTS_ROOT}/sherlock.json
                    --documents ${CMAKE_CURRENT_BINARY_DIR}/SherlockDocumentsCorpus/DoesNotExist.db)
    set_tests_properties(Sherlock.QueryNoDocuments PROPERTIES
            FIXTURES_REQUIRED SherlockStore
            PASS_REGULAR_EXPRESSION "layer 3: not built")

    if(SHERLOCK_IDA_FOUND AND SHERLOCK_DYLIBS)
        # One real image decompiled once; --resume makes every later run read the store instead.
        add_test(NAME Sherlock.HexRaysExport
                COMMAND $<TARGET_FILE:SherlockHexRays>
                        --image ${SHERLOCK_DYLIBS}/DesignLibrary
                        --store ${SHERLOCK_STORE}/Images
                        --name DesignLibrary
                        --image-path /System/Library/PrivateFrameworks/DesignLibrary.framework/Versions/A/DesignLibrary
                        --build ${SHERLOCK_BUILD} --keep-database --resume)
        set_tests_properties(Sherlock.HexRaysExport PROPERTIES
                FIXTURES_SETUP SherlockHexRays TIMEOUT 3600
                ENVIRONMENT "PATH=${SHERLOCK_IDA_DIR};$ENV{PATH}")

        add_test(NAME Sherlock.HexRaysResume
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysResumeGate.py)
        set_tests_properties(Sherlock.HexRaysResume PROPERTIES
                ENVIRONMENT "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_RESUME_IMAGE=${SHERLOCK_DYLIBS}/TemplateKit"
                TIMEOUT 1800)

        add_test(NAME Sherlock.HexRaysGrep
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysGrepGate.py)
        set_tests_properties(Sherlock.HexRaysGrep PROPERTIES
                FIXTURES_REQUIRED SherlockHexRays TIMEOUT 1800
                ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>;SHERLOCK_STORE=${SHERLOCK_STORE}")

        add_test(NAME Sherlock.HexRaysQueryGate
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysQueryGate.py)
        set_tests_properties(Sherlock.HexRaysQueryGate PROPERTIES
                FIXTURES_REQUIRED "SherlockStore;SherlockHexRays" TIMEOUT 1800
                ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>;SHERLOCK_STORE=${SHERLOCK_STORE};SHERLOCK_CACHE=${SHERLOCK_CACHE}")

        # HexRaysProjection.py is read by a person, not by an exit code, and stays a manual run:
        #   $env:SHERLOCK_STORE / SHERLOCK_CACHE / SHERLOCK_CORPUS_DYLIBS, then python tests/HexRaysProjection.py

        add_test(NAME Sherlock.HexRaysWorkerGate
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysWorkerGate.py)
        set_tests_properties(Sherlock.HexRaysWorkerGate PROPERTIES
                FIXTURES_REQUIRED SherlockHexRays
                ENVIRONMENT "SHERLOCK_STORE=${SHERLOCK_STORE}")
    endif()
endif()
