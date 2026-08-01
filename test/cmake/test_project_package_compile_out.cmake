cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR OR
   NOT DEFINED PULP_GENERATOR)
    message(FATAL_ERROR
        "PULP_SOURCE_DIR, PULP_BUILD_DIR, and PULP_GENERATOR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/project-package-compile-out")
set(_compile_out_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}"
    -B "${_compile_out_build}"
    -G "${PULP_GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release
    -DPULP_ENABLE_PROJECT_PACKAGE=OFF
    -DPULP_BUILD_TESTS=ON
    -DPULP_BUILD_EXAMPLES=OFF
    -DPULP_ENABLE_GPU=OFF
    -DPULP_ENABLE_DESIGN_IMPORT=ON
    -DPULP_FETCHCONTENT_UPDATES_DISCONNECTED=ON)
if(PULP_GENERATOR_PLATFORM)
    list(APPEND _configure_command -A "${PULP_GENERATOR_PLATFORM}")
endif()
if(PULP_GENERATOR_TOOLSET)
    list(APPEND _configure_command -T "${PULP_GENERATOR_TOOLSET}")
endif()
if(PULP_C_COMPILER)
    list(APPEND _configure_command "-DCMAKE_C_COMPILER=${PULP_C_COMPILER}")
endif()
if(PULP_CXX_COMPILER)
    list(APPEND _configure_command "-DCMAKE_CXX_COMPILER=${PULP_CXX_COMPILER}")
endif()

execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "PULP_ENABLE_PROJECT_PACKAGE=OFF did not configure coherently "
        "(${_configure_result})\n${_configure_output}\n${_configure_error}")
endif()

file(READ "${_compile_out_build}/CMakeCache.txt" _cache)
if(NOT _cache MATCHES
   "(^|\n)PULP_ENABLE_PROJECT_PACKAGE:BOOL=OFF($|\n)")
    message(FATAL_ERROR
        "compile-out fixture did not preserve PULP_ENABLE_PROJECT_PACKAGE=OFF")
endif()

# TargetDirectories is CMake's generated target inventory for both Makefile and
# Ninja generators. It lets this negative gate prove target absence without
# asking a missing target to build (or accidentally starting a broad build when
# a regression makes that target exist).
set(_target_directories
    "${_compile_out_build}/CMakeFiles/TargetDirectories.txt")
if(NOT EXISTS "${_target_directories}")
    message(FATAL_ERROR
        "compile-out fixture did not generate ${_target_directories}")
endif()
file(READ "${_target_directories}" _targets)
foreach(_forbidden_target IN ITEMS
        pulp-project-package
        pulp-tool-timeline
        pulp-cli
        pulp-mcp)
    if(_targets MATCHES
       "(^|[/\\])${_forbidden_target}\\.dir([/\\]|$)")
        message(FATAL_ERROR
            "PULP_ENABLE_PROJECT_PACKAGE=OFF still generated "
            "${_forbidden_target}")
    endif()
endforeach()

# Configuration generates the complete install/export program even though this
# proof intentionally builds nothing. Inspecting that generated program is a
# faster and stricter negative check than installing a broad SDK and looking for
# leftovers: a stale rule fails before it can ship an archive, target, or header.
file(GLOB_RECURSE _install_metadata LIST_DIRECTORIES FALSE
    "${_compile_out_build}/*cmake_install.cmake"
    "${_compile_out_build}/CMakeFiles/Export/*.cmake")
if(NOT _install_metadata)
    message(FATAL_ERROR "compile-out fixture generated no install/export metadata")
endif()
set(_install_program "")
foreach(_metadata IN LISTS _install_metadata)
    file(READ "${_metadata}" _metadata_text)
    string(APPEND _install_program "\n${_metadata_text}")
endforeach()
foreach(_forbidden IN ITEMS
        "Pulp::project-package"
        "core/project_package/include"
        "libpulp-project-package")
    string(FIND "${_install_program}" "${_forbidden}" _forbidden_offset)
    if(NOT _forbidden_offset EQUAL -1)
        message(FATAL_ERROR
            "PULP_ENABLE_PROJECT_PACKAGE=OFF left '${_forbidden}' in generated "
            "install/export metadata")
    endif()
endforeach()

set(_package_config "${_compile_out_build}/PulpConfig.cmake")
if(NOT EXISTS "${_package_config}")
    message(FATAL_ERROR "compile-out fixture generated no PulpConfig.cmake")
endif()
file(READ "${_package_config}" _package_config_text)
string(FIND "${_package_config_text}" "pulp-project-package" _component_offset)
if(NOT _component_offset EQUAL -1)
    message(FATAL_ERROR
        "disabled project-package remains advertised as an installed component")
endif()

message(STATUS
    "project_package_compile_out_verified=true target=false header=false "
    "install=false export=false component=false dependent_tools=false")
