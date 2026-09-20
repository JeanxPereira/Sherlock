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
set(SHERLOCK_TEST_ENV
    "SHERLOCK_CACHE=${SHERLOCK_CACHE_DIR};SHERLOCK_STORE=${SHERLOCK_STORE_DIR};SHERLOCK_TOWERS=${SHERLOCK_REPO}/References/scripts/towers.json")

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
sherlock_gate(FactsGates LINKS Sherlock::Facts)
sherlock_gate(DocumentIndexGates LINKS Sherlock::DocumentIndex)
sherlock_gate(HexRaysStoreGates LINKS Sherlock::HexRaysExport)

# SealDump: not a gate itself (no Sherlock.SealDump ctest entry) -- the small driver
# DocumentIndexParity.py runs over the whole Source/ tree, standing in for the CLI's own
# "build docs" until Task 6 lands (see DocumentIndexParity.py's own header for the deviation).
add_executable(SherlockSealDump ${CMAKE_CURRENT_LIST_DIR}/SealDump.cpp)
target_link_libraries(SherlockSealDump PRIVATE Sherlock::DocumentIndex)
sherlock_compile_options(SherlockSealDump)

add_test(NAME Sherlock.DocumentIndexParity
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/DocumentIndexParity.py
                --dump $<TARGET_FILE:SherlockSealDump> --repo ${SHERLOCK_REPO})

sherlock_gate(CliGates LINKS Sherlock::Facts Sherlock::HexRaysExport)
target_sources(SherlockTestCliGates PRIVATE
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/Arguments.cpp
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/Towers.cpp
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/Demangler.cpp)
target_include_directories(SherlockTestCliGates PRIVATE ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/include)
target_link_libraries(SherlockTestCliGates PRIVATE nlohmann_json::nlohmann_json)

sherlock_gate(DocumentsGates LINKS Sherlock::Store Sherlock::DocumentIndex Sherlock::DyldSharedCache)
target_sources(SherlockTestDocumentsGates PRIVATE
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/Documents.cpp)
target_include_directories(SherlockTestDocumentsGates PRIVATE
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/include)

add_executable(SherlockDocumentsFixture ${CMAKE_CURRENT_LIST_DIR}/DocumentsFixture.cpp)
target_link_libraries(SherlockDocumentsFixture PRIVATE Sherlock::Store)
sherlock_compile_options(SherlockDocumentsFixture)

add_test(NAME Sherlock.LaudoExactOutputGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/LaudoExactOutputGate.py
                $<TARGET_FILE:Sherlock> $<TARGET_FILE:SherlockDocumentsFixture>
                ${CMAKE_CURRENT_BINARY_DIR}/LaudoExactOutputScratch)

# Sherlock.BuildTowers fills the real store once; every query test below requires it and none rebuilds it.
add_test(NAME Sherlock.BuildTowers
        COMMAND $<TARGET_FILE:Sherlock> build facts --towers --resume --cache ${SHERLOCK_CACHE_DIR} --store ${SHERLOCK_STORE_DIR})
set_tests_properties(Sherlock.BuildTowers PROPERTIES FIXTURES_SETUP SherlockStore TIMEOUT 3600)

function(sherlock_query_test name)
    cmake_parse_arguments(ARG "" "MATCH" "" ${ARGN})
    add_test(NAME Sherlock.${name} COMMAND $<TARGET_FILE:Sherlock> ${ARG_UNPARSED_ARGUMENTS}
            --cache ${SHERLOCK_CACHE_DIR} --store ${SHERLOCK_STORE_DIR})
    set_tests_properties(Sherlock.${name} PROPERTIES
        FIXTURES_REQUIRED SherlockStore
        PASS_REGULAR_EXPRESSION "${ARG_MATCH}")
endfunction()

sherlock_query_test(QueryOwner    q 0x240622d98                          MATCH "DesignLibrary")
sherlock_query_test(QueryHeader   q 0x240622d98                          MATCH "build 26A5416b")
sherlock_query_test(QueryOwnerCoverage q 0x240622d98                     MATCH "verdict: FOUND 1  coverage 22798615/22799028")
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
        --cache ${SHERLOCK_CACHE_DIR} --store ${SHERLOCK_STORE_DIR})
set_tests_properties(Sherlock.QueryIncomplete PROPERTIES FIXTURES_REQUIRED SherlockStore)

add_test(NAME Sherlock.QueryCallersWithoutCache
        COMMAND $<TARGET_FILE:Sherlock> callers 0x2230edebc --store ${SHERLOCK_STORE_DIR})
set_tests_properties(Sherlock.QueryCallersWithoutCache PROPERTIES
        FIXTURES_REQUIRED SherlockStore
        PASS_REGULAR_EXPRESSION "0x240679e58")

foreach(scenario IN ITEMS meta-prepare meta-step images)
    add_test(NAME Sherlock.QueryHeaderFailure.${scenario}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/QueryHeaderFailure.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR} ${scenario})
endforeach()

add_test(NAME Sherlock.Parity
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/Parity.py
                --sherlock $<TARGET_FILE:Sherlock> --store ${SHERLOCK_STORE_DIR})
set_tests_properties(Sherlock.Parity PROPERTIES FIXTURES_REQUIRED SherlockStore TIMEOUT 600)

add_test(NAME Sherlock.ParityGates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/ParityGates.py
                ${CMAKE_CURRENT_BINARY_DIR}/ParityGateScratch)

add_test(NAME Sherlock.FinalFixGates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/FinalFixGates.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR}/FinalFixGateScratch)

# q's two layer-3 blocks (cited by / sealed at) and status's layer-3 line (task 8). The behavior
# itself is covered corpus-free by DocumentLayerGates below; these ctest entries additionally
# prove it end to end against the real corpus's own positive controls.
# Queries.cpp reads layer 2 and disassembles for --asm, so this target carries both modules.
sherlock_gate(DocumentLayerGates LINKS Sherlock::Store Sherlock::DocumentIndex Sherlock::DyldSharedCache Sherlock::Facts Sherlock::HexRaysExport)
target_sources(SherlockTestDocumentLayerGates PRIVATE
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/Queries.cpp)
target_include_directories(SherlockTestDocumentLayerGates PRIVATE
    ${SHERLOCK_REPO}/tools/Sherlock/Source/SherlockCli/include)

set(SHERLOCK_DOCUMENTS_CORPUS ${CMAKE_CURRENT_BINARY_DIR}/SherlockDocumentsCorpus/Documents.db)

# Pure markdown/source parsing over the real repository -- no cache or Facts store needed, so
# this is cheap (a few seconds) even though every other query test below depends on it.
add_test(NAME Sherlock.BuildDocuments
        COMMAND $<TARGET_FILE:Sherlock> build docs --repo ${SHERLOCK_REPO} --documents ${SHERLOCK_DOCUMENTS_CORPUS})
set_tests_properties(Sherlock.BuildDocuments PROPERTIES FIXTURES_SETUP SherlockDocuments TIMEOUT 120)

# Measured 2026-09-18 by `python tests/Sherlock/measure_document_counts.py <repo root>` -- a
# corpus fact, not a code fact: re-run that command and update these three numbers by hand
# whenever docs/re, docs/concepts or Source/'s seals grow or shrink enough to move them.
# BIN_SEALS moved 2144 -> 2207 in this same measurement: WalkSource (and measure_document_counts.py's
# own walk) now carry the shader extensions (.frag/.vert/.glsl) that SealDump and lint_seals.py
# already scanned -- the built store was missing every shader seal until this fix.
set(SHERLOCK_DOCUMENTS_RE_SECTIONS 2719)
set(SHERLOCK_DOCUMENTS_CONCEPT_SECTIONS 211)
set(SHERLOCK_DOCUMENTS_BIN_SEALS 2207)

add_test(NAME Sherlock.DocumentsExactCounts
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/DocumentsExactCounts.py
                ${SHERLOCK_DOCUMENTS_CORPUS}
                ${SHERLOCK_DOCUMENTS_RE_SECTIONS} ${SHERLOCK_DOCUMENTS_CONCEPT_SECTIONS}
                ${SHERLOCK_DOCUMENTS_BIN_SEALS})
set_tests_properties(Sherlock.DocumentsExactCounts PROPERTIES FIXTURES_REQUIRED SherlockDocuments)

function(sherlock_documents_query_test name)
    cmake_parse_arguments(ARG "" "MATCH" "" ${ARGN})
    add_test(NAME Sherlock.${name} COMMAND $<TARGET_FILE:Sherlock> ${ARG_UNPARSED_ARGUMENTS}
            --cache ${SHERLOCK_CACHE_DIR} --store ${SHERLOCK_STORE_DIR} --documents ${SHERLOCK_DOCUMENTS_CORPUS})
    set_tests_properties(Sherlock.${name} PROPERTIES
        FIXTURES_REQUIRED "SherlockStore;SherlockDocuments"
        PASS_REGULAR_EXPRESSION "${ARG_MATCH}")
endfunction()

# The Measured Facts table's own positive controls: the DesignLibrary 13-layer table's citations,
# and the SnippetSizeConstants.h clean BIN-tagged seal. Named like the existing Query* tests, so --
# like them -- excluded from a run that skips the slow real-corpus facts build (`-E "...|Query|..."`);
# verified directly against the built binary instead (see this task's report).
sherlock_documents_query_test(QueryCitedBy  q 0x27c198c20 MATCH "cited by.*resolvelayers-recon")
sherlock_documents_query_test(QuerySealedAt q 0x22695fe48 MATCH "sealed at.*SnippetSizeConstants")

# 0x27c198c20's OWN seal (Source/DesignLibrary/LayerResolver.cpp:864) sits BEFORE its own BIN
# tag in the sentence, so it is permanently a no-address seal (tools/seals-baseline.txt:45) --
# "sealed at" for this exact address is always 0, and that 0 must never print bare: the
# blind-spot line must name a nonzero count of no-address seals the tree actually carries.
sherlock_documents_query_test(QuerySealedAtBlindSpot q 0x27c198c20 MATCH "sealed at blind spot: [1-9][0-9]* seal")

# The shader-seal control: WalkSource's inclusion rule (DocumentIndex::HasSealExtension) must carry
# .frag/.vert/.glsl into Documents.db, or this seal -- real, decoded, cited by
# 2026-09-17-siri-wave-archive-renderer-parity.md -- goes missing from the built store while
# SealDump/DocumentIndexParity.py, which walk the same files directly, keep passing. That is
# exactly the drift this gate exists to catch: narrow WalkSource's extension list back to
# .h/.hpp/.cpp and this FAILS with no "sealed at" line for 0x24058bd88 at all.
sherlock_documents_query_test(QuerySealedAtShader q 0x24058bd88 MATCH "sealed at Source/DesignLibrary/shaders/SiriEffect.frag:4")

# find and laudo themselves, against the same real corpus -- QueryCitedBy/QuerySealedAt above
# prove q's layer-3 lines; these prove the commands Task 7 exists for.
sherlock_documents_query_test(LaudoOutline   laudo 2026-08-22-resolvelayers-recon  MATCH "7.*LayerResolver")
sherlock_documents_query_test(LaudoSection   laudo 2026-08-22-resolvelayers-recon §7 MATCH "0x27c198c20")
sherlock_documents_query_test(FindShadowPool find "shadow pool"                    MATCH "systembanner-geometria")

# laudo's own staleness gate (decision 8): a self-contained scratch repo, built and mutated
# with the real Sherlock.exe -- not the SherlockDocuments fixture above, since it needs to edit
# a file's content and mtime AFTER the build and observe the CLI itself refuse the stale text.
add_test(NAME Sherlock.LaudoStalenessGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/LaudoStalenessGate.py
                $<TARGET_FILE:Sherlock> ${CMAKE_CURRENT_BINARY_DIR}/LaudoStalenessScratch)

# Deliberately does NOT require SherlockDocuments, and points --documents at a path that is
# guaranteed never to exist -- proving decision 4's fallback cannot depend on ctest's run order
# relative to Sherlock.BuildDocuments, which writes to a fixed, shared path other tests also read.
add_test(NAME Sherlock.QueryNoDocuments
        COMMAND $<TARGET_FILE:Sherlock> q 0x240622d98 --cache ${SHERLOCK_CACHE_DIR} --store ${SHERLOCK_STORE_DIR}
                --documents ${CMAKE_CURRENT_BINARY_DIR}/SherlockDocumentsCorpus/DoesNotExist.db)
set_tests_properties(Sherlock.QueryNoDocuments PROPERTIES
        FIXTURES_REQUIRED SherlockStore
        PASS_REGULAR_EXPRESSION "layer 3: not built")

# status's layer 3, over the real, already-built Facts store and Documents.db. No "Query" in the
# name, so -- unlike the two tests above -- this DOES run in the reduced ctest pass: it reads the
# Facts Catalog.db Sherlock.BuildTowers produced once (every session shares that persisted store;
# this never rebuilds it), so it needs no facts-store fixture of its own.
add_test(NAME Sherlock.StatusLayerThree
        COMMAND $<TARGET_FILE:Sherlock> status --store ${SHERLOCK_STORE_DIR} --documents ${SHERLOCK_DOCUMENTS_CORPUS})
set_tests_properties(Sherlock.StatusLayerThree PROPERTIES
        FIXTURES_REQUIRED SherlockDocuments
        PASS_REGULAR_EXPRESSION "changed: [0-9]+, added: [0-9]+, removed: [0-9]+")

# The parent of `build hexrays` is driven with a stand-in worker, so its three behaviours -- the
# resume reset, a failed image, the free-space floor -- are gated on every machine, with or
# without IDA.
add_test(NAME Sherlock.HexRaysBuildGate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysBuildGate.py)
set_tests_properties(Sherlock.HexRaysBuildGate PROPERTIES
        ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>" TIMEOUT 600)

# Layer 2's gates exist only where an SDK matching the installed decompiler was found: the
# worker target does not exist otherwise, and a gate over a target that was never built would
# report the absence of IDA as a defect in Sherlock.
if(SHERLOCK_IDA_FOUND)
    add_test(NAME Sherlock.HexRaysSdkGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysSdkGate.py)
    set_tests_properties(Sherlock.HexRaysSdkGate PROPERTIES ENVIRONMENT
            "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_IDA_API=${SHERLOCK_IDA_API}")

    # The mutation of the pairing check, kept as a gate instead of run by hand: script mode
    # exercises the module alone, so it costs a second and never touches the dependencies.
    add_test(NAME Sherlock.HexRaysSdkRefusal
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysSdkRefusalGate.py)
    set_tests_properties(Sherlock.HexRaysSdkRefusal PROPERTIES ENVIRONMENT
            "CMAKE_COMMAND=${CMAKE_COMMAND};SHERLOCK_IDA_SDK_MODULE=${SHERLOCK_REPO}/tools/Sherlock/cmake/SherlockIdaSdk.cmake;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_IDA_API=${SHERLOCK_IDA_API}")

    # Sherlock.HexRaysExport decompiles one real image once, the way Sherlock.BuildTowers fills the
    # facts store once: --resume makes every later run read the store instead of paying the
    # minutes again, and the gate below only reads what it left. The worker needs IDA's DLLs on
    # PATH, since its imports resolve at process start.
    add_test(NAME Sherlock.HexRaysExport
            COMMAND $<TARGET_FILE:SherlockHexRays>
                    --image ${SHERLOCK_CORPUS}/${SHERLOCK_BUILD}/extracted/dylibs/DesignLibrary
                    --store ${SHERLOCK_STORE_DIR}/Images
                    --name DesignLibrary
                    --image-path /System/Library/PrivateFrameworks/DesignLibrary.framework/Versions/A/DesignLibrary
                    --build ${SHERLOCK_BUILD} --keep-database --resume)
    set_tests_properties(Sherlock.HexRaysExport PROPERTIES
            FIXTURES_SETUP SherlockHexRays TIMEOUT 3600
            ENVIRONMENT "PATH=${SHERLOCK_IDA_DIR};$ENV{PATH}")

    add_test(NAME Sherlock.HexRaysLeftover
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysLeftoverGate.py)
    set_tests_properties(Sherlock.HexRaysLeftover PROPERTIES
            ENVIRONMENT "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR}"
            TIMEOUT 600)

    # What an interruption costs. The gate copies the smallest tower out of the corpus, kills the
    # worker once a batch has landed and runs it again with --resume, so it needs no fixture and
    # leaves no database beside the real image. TemplateKit decompiles in about twenty seconds,
    # which is the head start the kill needs.
    add_test(NAME Sherlock.HexRaysResume
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysResumeGate.py)
    set_tests_properties(Sherlock.HexRaysResume PROPERTIES
            ENVIRONMENT "SHERLOCK_HEXRAYS_WORKER=$<TARGET_FILE:SherlockHexRays>;SHERLOCK_IDA_DIR=${SHERLOCK_IDA_DIR};SHERLOCK_RESUME_IMAGE=${SHERLOCK_CORPUS}/${SHERLOCK_BUILD}/extracted/dylibs/TemplateKit"
            TIMEOUT 1800)

    # grep needs no IDA of its own -- it reads stores -- but it needs a store with layer 2 in it,
    # and the export fixture is what produces one.
    add_test(NAME Sherlock.HexRaysGrep
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysGrepGate.py)
    set_tests_properties(Sherlock.HexRaysGrep PROPERTIES
            FIXTURES_REQUIRED SherlockHexRays
            TIMEOUT 1800
            ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>;SHERLOCK_STORE=${SHERLOCK_STORE_DIR}")

    # `fn` reads both layers, so it needs the facts store AND one exported image.
    add_test(NAME Sherlock.HexRaysQueryGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysQueryGate.py)
    set_tests_properties(Sherlock.HexRaysQueryGate PROPERTIES
            FIXTURES_REQUIRED "SherlockStore;SherlockHexRays"
            TIMEOUT 1800
            ENVIRONMENT "SHERLOCK_CLI=$<TARGET_FILE:Sherlock>;SHERLOCK_STORE=${SHERLOCK_STORE_DIR};SHERLOCK_CACHE=${SHERLOCK_CACHE_DIR}")

    # HexRaysProjection.py is the phase-3 gate and NOT a ctest entry, the way SealDump is not:
    # it needs five exported images and its verdict is read by a person, not by an exit code in a
    # suite. Run it by hand once the export is done:
    #
    #   $env:SHERLOCK_STORE="<corpus>/<build>/Sherlock"
    #   $env:SHERLOCK_CACHE="<corpus>/<build>/dsc/<build>__MacOS"
    #   $env:SHERLOCK_CORPUS_DYLIBS="<corpus>/<build>/extracted/dylibs"
    #   python tests/Sherlock/HexRaysProjection.py
    #
    # It exits 2 while fewer than five images are exported, which is the honest answer to being
    # asked for a projection that cannot yet be made.

    add_test(NAME Sherlock.HexRaysWorkerGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/HexRaysWorkerGate.py)
    set_tests_properties(Sherlock.HexRaysWorkerGate PROPERTIES
            FIXTURES_REQUIRED SherlockHexRays
            ENVIRONMENT "SHERLOCK_STORE=${SHERLOCK_STORE_DIR}")
endif()
