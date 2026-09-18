# Sherlock — tools/Sherlock/cmake/SherlockIdaSdk.cmake
# Selects an IDA SDK whose decompiler API magic matches an installed IDA's, and says why when none does.

set(SHERLOCK_IDA_SDK "" CACHE PATH "IDA C++ SDK root (the directory holding include/ and lib/)")
set(SHERLOCK_IDA_DIR "" CACHE PATH "IDA installation whose DLLs the worker loads")
set(SHERLOCK_IDA_FOUND FALSE)
# The decompiler API both sides were matched on, handed to the gate so the binary can be held
# against the check rather than the check standing alone.
set(SHERLOCK_IDA_API "")

# The SDK states the API version it speaks as the low word of HEXRAYS_API_MAGIC.
function(_sherlock_sdk_magic sdk out)
    set(${out} "" PARENT_SCOPE)
    if(NOT EXISTS "${sdk}/include/hexrays.hpp")
        return()
    endif()
    file(STRINGS "${sdk}/include/hexrays.hpp" _line REGEX "HEXRAYS_API_MAGIC *=")
    if(_line MATCHES "0x00DEC0DE0000000([0-9A-Fa-f])")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
endfunction()

# The decompiler carries the same constant as a literal, so the pair can be checked before a
# single line is compiled. Matching it byte-wise is the only check that predicts
# init_hexrays_plugin(): a 9.4 SDK links against a 9.2 runtime, opens a database, and fails
# only the handshake -- the SDK's version number never says that will happen.
function(_sherlock_runtime_has_magic idadir magic out)
    set(${out} FALSE PARENT_SCOPE)
    file(GLOB _decompilers "${idadir}/plugins/hex*.dll")
    string(TOLOWER "0${magic}000000dec0de00" _needle) # 0x00DEC0DE0000000N, little-endian
    foreach(_dll IN LISTS _decompilers)
        file(READ "${_dll}" _bytes HEX)
        string(FIND "${_bytes}" "${_needle}" _at)
        if(NOT _at EQUAL -1)
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

if(SHERLOCK_IDA_SDK AND SHERLOCK_IDA_DIR)
    _sherlock_sdk_magic("${SHERLOCK_IDA_SDK}" _sherlock_magic)
    if(NOT _sherlock_magic)
        message(STATUS "Sherlock: layer 2 off -- no readable hexrays.hpp under ${SHERLOCK_IDA_SDK}")
    else()
        _sherlock_runtime_has_magic("${SHERLOCK_IDA_DIR}" "${_sherlock_magic}" _sherlock_matches)
        if(_sherlock_matches)
            set(SHERLOCK_IDA_FOUND TRUE)
            set(SHERLOCK_IDA_API "${_sherlock_magic}")
            message(STATUS "Sherlock: layer 2 on -- SDK decompiler API ${_sherlock_magic} matches ${SHERLOCK_IDA_DIR}")
        else()
            message(FATAL_ERROR
                "Sherlock: the SDK at ${SHERLOCK_IDA_SDK} speaks decompiler API ${_sherlock_magic}, and no "
                "decompiler in ${SHERLOCK_IDA_DIR}/plugins carries it. The worker would link, run, and then "
                "fail init_hexrays_plugin() with no reason given. Point SHERLOCK_IDA_SDK at the SDK matching "
                "that install (the SDK repository's git tags name the versions), or SHERLOCK_IDA_DIR at the "
                "install matching that SDK.")
        endif()
    endif()
else()
    message(STATUS "Sherlock: layer 2 off -- SHERLOCK_IDA_SDK / SHERLOCK_IDA_DIR not set")
endif()
