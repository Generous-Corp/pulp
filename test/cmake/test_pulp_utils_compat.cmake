cmake_minimum_required(VERSION 3.24)

# Installed-SDK compatibility proof for PulpUtils.cmake.  This intentionally
# discovers the shim's sibling includes instead of naming an implementation
# module: a future concern split may add or rename private helpers, and the
# install must continue to carry every file the shim resolves at configure
# time.

foreach(_required PULP_BUILD_DIR PULP_SOURCE_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

set(_root "${PULP_BUILD_DIR}/pulp-utils-compat-smoke")
set(_prefix "${_root}/prefix")
set(_consumer_source "${_root}/consumer")
set(_consumer_build "${_root}/consumer-build")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_consumer_source}")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "PulpUtils compatibility SDK install failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(GLOB _installed_utils LIST_DIRECTORIES false
    "${_prefix}/lib*/cmake/Pulp/PulpUtils.cmake")
if(NOT _installed_utils)
    message(FATAL_ERROR
        "Installed PulpUtils.cmake not found under ${_prefix}/lib*/cmake/Pulp")
endif()
list(GET _installed_utils 0 _installed_utils)
get_filename_component(_installed_cmake_dir "${_installed_utils}" DIRECTORY)
file(READ "${_installed_utils}" _utils_text)

# Every sibling include resolved from the shim must be installed beside it.
# PulpUtils also includes helpers with a plain module name; those are not
# relative files and are intentionally outside this check.
file(STRINGS "${_installed_utils}" _relative_include_lines
    REGEX "CMAKE_CURRENT_LIST_DIR.*\\.cmake")
set(_relative_helpers "")
foreach(_line IN LISTS _relative_include_lines)
    string(REGEX MATCH
        "CMAKE_CURRENT_LIST_DIR}/([^\" )]+\\.cmake)" _match "${_line}")
    if(_match)
        list(APPEND _relative_helpers "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _relative_helpers)
if(NOT _relative_helpers)
    message(FATAL_ERROR
        "Installed PulpUtils.cmake has no relative helper includes; the shim "
        "may have lost its compatibility module wiring")
endif()
foreach(_helper IN LISTS _relative_helpers)
    if(NOT EXISTS "${_installed_cmake_dir}/${_helper}")
        message(FATAL_ERROR
            "Installed PulpUtils shim includes ${_helper}, but it was not installed")
    endif()
endforeach()

# Keep this list explicit: these are the functions that downstream projects
# have historically received by including PulpUtils (including the two helper
# functions imported by the shim's existing AppIcon/MidiTuning includes).
set(_public_functions
    pulp_add_plugin
    pulp_add_plugin_bundle
    pulp_add_reload_logic
    pulp_reload_host
    pulp_reload_host_ui
    pulp_use_kit_ui
    pulp_app_icon
    pulp_enable_midi_tuning_provider)

file(WRITE "${_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpUtilsCompatConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED)
include(PulpUtils)

set(_expected_functions
    pulp_add_plugin
    pulp_add_plugin_bundle
    pulp_add_reload_logic
    pulp_reload_host
    pulp_reload_host_ui
    pulp_use_kit_ui
    pulp_app_icon
    pulp_enable_midi_tuning_provider)
foreach(_function IN LISTS _expected_functions)
    if(NOT COMMAND ${_function})
        message(FATAL_ERROR
            "Installed PulpUtils compatibility shim does not provide ${_function}()")
    endif()
endforeach()

# This is the stable target-selection surface used by PulpUtils.  Record it in
# sorted order so configure receipts remain byte-stable across generators.
set(_required_targets
    Pulp::format
    Pulp::view
    Pulp::audio
    Pulp::midi
    Pulp::standalone)
set(_missing_targets "")
foreach(_target IN LISTS _required_targets)
    if(NOT TARGET ${_target})
        list(APPEND _missing_targets ${_target})
    endif()
endforeach()
if(_missing_targets)
    string(REPLACE ";" ", " _missing_text "${_missing_targets}")
    message(FATAL_ERROR "Installed Pulp target surface is incomplete: ${_missing_text}")
endif()

list(SORT _required_targets)
string(REPLACE ";" "\n" _target_receipt "${_required_targets}")
file(WRITE "${PULP_UTILS_TARGET_RECEIPT}" "${_target_receipt}\n")
message(STATUS "pulp_utils_compat_commands=8")
message(STATUS "pulp_utils_compat_targets=${_required_targets}")
]=])

set(_configure_args
    -S "${_consumer_source}"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_installed_cmake_dir}"
    "-DCMAKE_BUILD_TYPE=${_config}"
    "-DPULP_UTILS_TARGET_RECEIPT=${_root}/target-list.txt")
if(_config STREQUAL "Debug")
    list(APPEND _configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed PulpUtils consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

if(NOT EXISTS "${_root}/target-list.txt")
    message(FATAL_ERROR "PulpUtils consumer did not write target-list.txt")
endif()
file(READ "${_root}/target-list.txt" _target_receipt)
if(NOT _target_receipt MATCHES "Pulp::audio" OR
   NOT _target_receipt MATCHES "Pulp::format" OR
   NOT _target_receipt MATCHES "Pulp::midi" OR
   NOT _target_receipt MATCHES "Pulp::standalone" OR
   NOT _target_receipt MATCHES "Pulp::view")
    message(FATAL_ERROR
        "PulpUtils target receipt is incomplete:\n${_target_receipt}")
endif()

message(STATUS
    "pulp_utils_compat_verified=true helpers=${_relative_helpers} "
    "target_receipt=${_root}/target-list.txt")
