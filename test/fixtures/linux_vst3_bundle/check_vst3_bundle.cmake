# Check a Linux VST3 bundle's layout: BUNDLE is <Name>.vst3, MODULE_NAME is
# <Name>. The module must sit at Contents/<machine>-linux/<Name>.so, where
# <machine> is what `uname -m` reports (the SDK's loader builds the path from
# it), be an ELF shared object that exports GetPluginFactory, and nothing but
# Contents may be a module at the top.
cmake_minimum_required(VERSION 3.24)

foreach(_required BUNDLE MODULE_NAME)
    if(NOT ${_required})
        message(FATAL_ERROR "vst3 bundle layout: ${_required} is required")
    endif()
endforeach()

if(NOT BUNDLE MATCHES "\\.vst3$")
    message(FATAL_ERROR "not a .vst3 bundle path: ${BUNDLE}")
endif()
if(NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "the bundle is not a directory: ${BUNDLE}")
endif()

execute_process(COMMAND uname -m OUTPUT_VARIABLE _machine OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT _machine)
    message(FATAL_ERROR "uname -m failed")
endif()

set(_module "${BUNDLE}/Contents/${_machine}-linux/${MODULE_NAME}.so")
if(NOT EXISTS "${_module}")
    file(GLOB_RECURSE _found "${BUNDLE}/*")
    message(FATAL_ERROR "no module at ${_module}; the bundle holds: ${_found}")
endif()

file(READ "${_module}" _magic LIMIT 4 HEX)
if(NOT _magic STREQUAL "7f454c46")
    message(FATAL_ERROR "${_module} is not an ELF file (${_magic})")
endif()

find_program(_nm NAMES nm llvm-nm REQUIRED)
execute_process(COMMAND "${_nm}" -D --defined-only "${_module}"
    OUTPUT_VARIABLE _symbols RESULT_VARIABLE _nm_rc)
if(NOT _nm_rc EQUAL 0 OR NOT _symbols MATCHES " T GetPluginFactory")
    message(FATAL_ERROR "${_module} does not export GetPluginFactory")
endif()

file(GLOB _top "${BUNDLE}/*")
foreach(_entry IN LISTS _top)
    get_filename_component(_leaf "${_entry}" NAME)
    if(NOT _leaf STREQUAL "Contents")
        message(FATAL_ERROR "unexpected entry beside Contents in the bundle: ${_leaf}")
    endif()
endforeach()
message(STATUS "VST3 bundle layout ok: ${_module}")
