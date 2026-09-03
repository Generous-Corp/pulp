if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR
        "test_cli_gpu_probe_link_group requires PULP_SOURCE_DIR and FIXTURE_DIR")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/source")
file(WRITE "${FIXTURE_DIR}/source/stub.cpp" "void pulp_link_fixture_stub() {}\n")
file(WRITE "${FIXTURE_DIR}/source/main.cpp" "int main() { return 0; }\n")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpCliGpuProbeLinkFixture LANGUAGES CXX)

# Generation is the behavior under test; no fixture is linked. Defining the
# feature explicitly lets every host exercise CMake's Linux rescan graph rules.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN
    "LINKER:--start-group" "LINKER:--end-group")
set(PULP_LINUX TRUE)
set(PULP_ENABLE_SCENE3D TRUE)

function(add_fixture_library target alias)
    add_library(${target} STATIC stub.cpp)
    add_library(${alias} ALIAS ${target})
endfunction()

add_fixture_library(pulp-render pulp::render)
add_fixture_library(pulp-scene pulp::scene)
add_fixture_library(pulp-view-core pulp::view-core)
add_fixture_library(pulp-view-script pulp::view-script)
add_fixture_library(pulp-view pulp::view)
add_fixture_library(pulp-gpu-audio pulp::gpu-audio)
add_fixture_library(pulp-tool-gpu-probe-recipes
    pulp::tool-gpu-probe-recipes)

target_link_libraries(pulp-view-core PUBLIC pulp::render)
target_link_libraries(pulp-view-script
    PUBLIC pulp::view-core
    PRIVATE pulp::render)
target_link_libraries(pulp-view PUBLIC pulp::view-script)
target_link_libraries(pulp-gpu-audio PUBLIC pulp::render)
target_link_libraries(pulp-tool-gpu-probe-recipes PRIVATE
    pulp::gpu-audio pulp::render pulp::view pulp::scene)

add_executable(pulp-cli main.cpp)
target_link_libraries(pulp-cli PRIVATE pulp::view pulp::render)
if(LEGACY_RECIPE_IN_GROUP)
    target_link_libraries(pulp-cli PRIVATE
        "$<LINK_GROUP:RESCAN,pulp::tool-gpu-probe-recipes,pulp::render,pulp::scene>")
else()
    include("${PULP_SOURCE_DIR}/tools/cli/cmake/PulpCliGpuProbeLink.cmake")
    pulp_link_cli_gpu_probe_recipes(pulp-cli)

    get_target_property(_pulp_cli_links pulp-cli LINK_LIBRARIES)
    set(_recipe_link_count 0)
    set(_rescan_group_count 0)
    foreach(_link IN LISTS _pulp_cli_links)
        if(_link STREQUAL "pulp::tool-gpu-probe-recipes")
            math(EXPR _recipe_link_count "${_recipe_link_count} + 1")
        elseif(_link MATCHES "LINK_GROUP:RESCAN")
            math(EXPR _rescan_group_count "${_rescan_group_count} + 1")
            if(NOT _link STREQUAL
                    "$<LINK_GROUP:RESCAN,pulp::render,pulp::scene>")
                message(FATAL_ERROR
                    "Unexpected CLI rescan group: ${_link}")
            endif()
        endif()
    endforeach()
    if(NOT _recipe_link_count EQUAL 1)
        message(FATAL_ERROR
            "Expected one standalone GPU recipe link: ${_pulp_cli_links}")
    endif()
    if(NOT _rescan_group_count EQUAL 1)
        message(FATAL_ERROR
            "Expected one exact render/scene rescan group: ${_pulp_cli_links}")
    endif()
endif()
]=])

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${FIXTURE_DIR}/source")
if(DEFINED PULP_GENERATOR AND NOT PULP_GENERATOR STREQUAL "")
    list(APPEND _configure_command -G "${PULP_GENERATOR}")
endif()
if(DEFINED PULP_GENERATOR_PLATFORM AND NOT PULP_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND _configure_command -A "${PULP_GENERATOR_PLATFORM}")
endif()
if(DEFINED PULP_GENERATOR_TOOLSET AND NOT PULP_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND _configure_command -T "${PULP_GENERATOR_TOOLSET}")
endif()
if(DEFINED PULP_CXX_COMPILER AND NOT PULP_CXX_COMPILER STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_CXX_COMPILER=${PULP_CXX_COMPILER}")
endif()
list(APPEND _configure_command
    -DCMAKE_BUILD_TYPE=Release
    "-DPULP_SOURCE_DIR=${PULP_SOURCE_DIR}")

execute_process(
    COMMAND ${_configure_command}
        -B "${FIXTURE_DIR}/good-build"
        -DLEGACY_RECIPE_IN_GROUP=OFF
    RESULT_VARIABLE _good_result
    OUTPUT_VARIABLE _good_stdout
    ERROR_VARIABLE _good_stderr)
if(NOT _good_result EQUAL 0)
    message(FATAL_ERROR
        "Separated recipe/render Scene3D link graph did not configure:\n"
        "${_good_stdout}${_good_stderr}")
endif()

execute_process(
    COMMAND ${_configure_command}
        -B "${FIXTURE_DIR}/legacy-build"
        -DLEGACY_RECIPE_IN_GROUP=ON
    RESULT_VARIABLE _legacy_result
    OUTPUT_VARIABLE _legacy_stdout
    ERROR_VARIABLE _legacy_stderr)
if(_legacy_result EQUAL 0)
    message(FATAL_ERROR
        "Legacy recipe-in-rescan-group graph unexpectedly configured")
endif()

set(_legacy_log "${_legacy_stdout}${_legacy_stderr}")
foreach(_required_text IN ITEMS
        "strongly connected component"
        "pulp-view"
        "pulp-gpu-audio"
        "RESCAN:{pulp::tool-gpu-probe-recipes,pulp::render,pulp::scene}")
    if(NOT _legacy_log MATCHES "${_required_text}")
        message(FATAL_ERROR
            "Legacy graph failed for an unexpected reason; missing "
            "'${_required_text}':\n${_legacy_log}")
    endif()
endforeach()

message(STATUS
    "CLI GPU recipe linkage keeps recipes outside the render/scene rescan group")
