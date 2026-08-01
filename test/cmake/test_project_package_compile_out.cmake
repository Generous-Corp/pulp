cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR OR
   NOT DEFINED PULP_GENERATOR)
    message(FATAL_ERROR
        "PULP_SOURCE_DIR, PULP_BUILD_DIR, and PULP_GENERATOR are required")
endif()
find_program(PULP_CTEST_COMMAND ctest REQUIRED)

set(_fixture_root "${PULP_BUILD_DIR}/project-package-compile-out")
set(_compile_out_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")
file(MAKE_DIRECTORY "${_compile_out_build}/.cmake/api/v1/query")
file(WRITE "${_compile_out_build}/.cmake/api/v1/query/codemodel-v2" "")

# This test is registered only by the default-ON build. Prove that its configure
# retained all three MCP test surfaces; checking TARGET while test/ is parsed is
# invalid because tools/mcp is added later in the root CMakeLists.
set(_outer_ctest_file "${PULP_BUILD_DIR}/test/CTestTestfile.cmake")
if(NOT EXISTS "${_outer_ctest_file}")
    message(FATAL_ERROR "default-ON build has no test/CTestTestfile.cmake")
endif()
file(READ "${_outer_ctest_file}" _outer_ctest)
foreach(_required_registration IN ITEMS
        pulp-mcp-binary-smoke
        pulp-test-mcp-server
        pulp-test-mcp-timeline-tools)
    string(FIND "${_outer_ctest}" "${_required_registration}"
        _registration_offset)
    if(_registration_offset EQUAL -1)
        message(FATAL_ERROR
            "default-ON build dropped MCP registration "
            "${_required_registration}")
    endif()
endforeach()

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}"
    -B "${_compile_out_build}"
    -G "${PULP_GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release
    -DPULP_ENABLE_PROJECT_PACKAGE=OFF
    -DPULP_BUILD_TESTS=ON
    -DPULP_BUILD_EXAMPLES=OFF
    -DPULP_ENABLE_GPU=ON
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
string(REPLACE "\\" "/" _targets_normalized "${_targets}")
foreach(_forbidden_target IN ITEMS
        pulp-project-package
        pulp-tool-timeline
        pulp-test-project-package
        pulp-test-timeline-agent
        pulp-test-mcp-timeline-tools
        pulp-test-cli-timeline)
    string(FIND "${_targets_normalized}" "/${_forbidden_target}.dir"
        _target_offset)
    if(NOT _target_offset EQUAL -1)
        message(FATAL_ERROR
            "PULP_ENABLE_PROJECT_PACKAGE=OFF still generated "
            "${_forbidden_target}")
    endif()
endforeach()
foreach(_required_target IN ITEMS
        pulp-cli
        pulp-mcp
        pulp-mcp-core
        pulp-test-mcp-server)
    string(FIND "${_targets_normalized}" "/${_required_target}.dir"
        _target_offset)
    if(_target_offset EQUAL -1)
        message(FATAL_ERROR
            "PULP_ENABLE_PROJECT_PACKAGE=OFF unexpectedly omitted "
            "${_required_target}")
    endif()
endforeach()

set(_inner_ctest_file "${_compile_out_build}/test/CTestTestfile.cmake")
if(NOT EXISTS "${_inner_ctest_file}")
    message(FATAL_ERROR "compile-out fixture generated no CTest inventory")
endif()
file(READ "${_inner_ctest_file}" _inner_ctest)
foreach(_required_registration IN ITEMS
        pulp-mcp-binary-smoke
        pulp-test-mcp-server)
    string(FIND "${_inner_ctest}" "${_required_registration}"
        _registration_offset)
    if(_registration_offset EQUAL -1)
        message(FATAL_ERROR
            "package-stripped build dropped independent MCP registration "
            "${_required_registration}")
    endif()
endforeach()
foreach(_forbidden_registration IN ITEMS
        cmake-timeline-sdk-consumer
        pulp-test-mcp-timeline-tools
        pulp-test-timeline-agent
        pulp-test-cli-timeline)
    string(FIND "${_inner_ctest}" "${_forbidden_registration}"
        _registration_offset)
    if(NOT _registration_offset EQUAL -1)
        message(FATAL_ERROR
            "package-stripped build retained Timeline registration "
            "${_forbidden_registration}")
    endif()
endforeach()

# The tools remain available, but their package-publishing commands must be
# compiled out. Query CMake's codemodel instead of generator-specific build
# files so the proof works with Makefiles, Ninja, Visual Studio, and Xcode.
file(GLOB _reply_indexes
    "${_compile_out_build}/.cmake/api/v1/reply/index-*.json")
list(LENGTH _reply_indexes _reply_index_count)
if(NOT _reply_index_count EQUAL 1)
    message(FATAL_ERROR
        "compile-out fixture expected one CMake file-api index, got "
        "${_reply_index_count}")
endif()
list(GET _reply_indexes 0 _reply_index)
file(READ "${_reply_index}" _index_json)
string(JSON _codemodel_file GET "${_index_json}" reply codemodel-v2 jsonFile)
file(READ "${_compile_out_build}/.cmake/api/v1/reply/${_codemodel_file}"
    _codemodel_json)
string(JSON _target_count LENGTH "${_codemodel_json}" configurations 0 targets)
math(EXPR _target_last "${_target_count} - 1")
set(_command_sources "")
foreach(_target_index RANGE 0 ${_target_last})
    string(JSON _target_name GET "${_codemodel_json}"
        configurations 0 targets ${_target_index} name)
    if(_target_name STREQUAL "pulp-cli" OR
       _target_name STREQUAL "pulp-mcp-core")
        string(JSON _target_file GET "${_codemodel_json}"
            configurations 0 targets ${_target_index} jsonFile)
        file(READ
            "${_compile_out_build}/.cmake/api/v1/reply/${_target_file}"
            _target_json)
        string(JSON _source_count LENGTH "${_target_json}" sources)
        math(EXPR _source_last "${_source_count} - 1")
        foreach(_source_index RANGE 0 ${_source_last})
            string(JSON _source_path GET "${_target_json}"
                sources ${_source_index} path)
            string(APPEND _command_sources "\n${_source_path}")
        endforeach()
    endif()
endforeach()
foreach(_forbidden_source IN ITEMS
        tools/cli/cmd_seq.cpp
        tools/mcp/mcp_timeline_tools.cpp)
    string(FIND "${_command_sources}" "${_forbidden_source}"
        _source_offset)
    if(NOT _source_offset EQUAL -1)
        message(FATAL_ERROR
            "package-stripped tool retained ${_forbidden_source}")
    endif()
endforeach()

# Build and execute the retained MCP surfaces. Metadata alone cannot prove that
# the legacy protocol test stopped including generated Timeline headers or that
# the real server smoke tolerates the intentionally absent Timeline tools.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_compile_out_build}"
        --target pulp-test-mcp-server pulp-mcp --parallel 8
    RESULT_VARIABLE _mcp_build_result
    OUTPUT_VARIABLE _mcp_build_output
    ERROR_VARIABLE _mcp_build_error)
if(NOT _mcp_build_result EQUAL 0)
    message(FATAL_ERROR
        "package-stripped retained MCP surfaces did not build "
        "(${_mcp_build_result})\n${_mcp_build_output}\n${_mcp_build_error}")
endif()
execute_process(
    COMMAND "${PULP_CTEST_COMMAND}" --test-dir "${_compile_out_build}"
        -L "^mcp-server$" --output-on-failure
    RESULT_VARIABLE _mcp_unit_result
    OUTPUT_VARIABLE _mcp_unit_output
    ERROR_VARIABLE _mcp_unit_error)
if(NOT _mcp_unit_result EQUAL 0)
    message(FATAL_ERROR
        "package-stripped MCP protocol tests failed "
        "(${_mcp_unit_result})\n${_mcp_unit_output}\n${_mcp_unit_error}")
endif()
execute_process(
    COMMAND "${PULP_CTEST_COMMAND}" --test-dir "${_compile_out_build}"
        -R "^pulp-mcp-binary-smoke$" --output-on-failure
    RESULT_VARIABLE _mcp_smoke_result
    OUTPUT_VARIABLE _mcp_smoke_output
    ERROR_VARIABLE _mcp_smoke_error)
if(NOT _mcp_smoke_result EQUAL 0)
    message(FATAL_ERROR
        "package-stripped pulp-mcp binary smoke failed "
        "(${_mcp_smoke_result})\n${_mcp_smoke_output}\n${_mcp_smoke_error}")
endif()

# Configuration generates the complete install/export program. Inspecting that
# generated program is stricter than installing a broad SDK and looking for
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
    "install=false export=false component=false tools=true "
    "timeline_commands=false default_mcp_tests=true")
