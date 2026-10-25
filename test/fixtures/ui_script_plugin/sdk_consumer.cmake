# Installed-SDK consumer: a plug-in declared with pulp_add_plugin(UI_SCRIPT),
# configured against `cmake --install` output through find_package(Pulp),
# opens its scripted editor. This is the path every product outside the Pulp
# tree takes, so the per-target editor-asset registration must ship with the
# installed CMake helpers and headers.
#
#   cmake -DPULP_BUILD_DIR=<build> -DFIXTURE_DIR=<this dir> -DPROBE=<exe>
#         -DEXPECT_GPU=<bool> -DCONFIG=<Release|Debug>
#         [-DPULP_PARENT_SANITIZER=<kind>] [-DPULP_PARENT_CXX_FLAGS=<flags>]
#         [-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=<flags>]
#         [-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=<flags>] -P sdk_consumer.cmake

cmake_minimum_required(VERSION 3.24)

foreach(_var PULP_BUILD_DIR FIXTURE_DIR PROBE)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()
if(NOT CONFIG)
    set(CONFIG Release)
endif()

set(_root "${PULP_BUILD_DIR}/ui-script-sdk-consumer")
set(_prefix "${_root}/prefix")
set(_source "${_root}/src")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_source}/ui")

execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
        --prefix "${_prefix}" --config "${CONFIG}"
    RESULT_VARIABLE _install_rc OUTPUT_VARIABLE _install_out ERROR_VARIABLE _install_err)
if(NOT _install_rc EQUAL 0)
    message(FATAL_ERROR "SDK install failed (${_install_rc}):\n${_install_out}\n${_install_err}")
endif()

file(COPY "${FIXTURE_DIR}/ui_script_gain.hpp" "${FIXTURE_DIR}/clap_entry.cpp"
     DESTINATION "${_source}")
file(COPY "${FIXTURE_DIR}/ui/main.js" DESTINATION "${_source}/ui")
file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(UiScriptSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Pulp CONFIG REQUIRED)
pulp_add_plugin(UiScriptSdkConsumer
    FORMATS           CLAP
    PLUGIN_NAME       "Pulp UI Script Fixture"
    BUNDLE_ID         "dev.pulp.test.ui-script-fixture"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PUiS"
    MANUFACTURER_CODE "PlUs"
    SOURCES           ui_script_gain.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_ui_script_gain
    UI_SCRIPT         ui/main.js
)
]=])

set(_configure_args -S "${_source}" -B "${_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}" "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${CONFIG}")
string(TOLOWER "${CONFIG}" _config_lower)
if(_config_lower STREQUAL "debug")
    list(APPEND _configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
# A sanitizer or coverage producer installs static libraries that reference
# its runtime: compile the consumer with the same instrumentation and link the
# plug-in module (and any executable) against that runtime.
set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
if(PULP_PARENT_SANITIZER)
    list(APPEND _configure_args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _configure_args "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    foreach(_kind EXE SHARED MODULE)
        list(APPEND _configure_args
            "-DCMAKE_${_kind}_LINKER_FLAGS=${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
    endforeach()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_rc OUTPUT_VARIABLE _configure_out ERROR_VARIABLE _configure_err)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR "consumer configure failed:\n${_configure_out}\n${_configure_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config "${CONFIG}"
        --target UiScriptSdkConsumer_CLAP --parallel 2
    RESULT_VARIABLE _build_rc OUTPUT_VARIABLE _build_out ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR "consumer build failed:\n${_build_out}\n${_build_err}")
endif()

# pulp_add_plugin puts CLAP bundles under <build>/CLAP; the bundle is the
# .clap directory, and the build tree also holds a <target>_Info.plist.clap file.
file(GLOB _clap_candidates LIST_DIRECTORIES true "${_build}/CLAP/*.clap")
set(_clap_binaries "")
foreach(_candidate IN LISTS _clap_candidates)
    if(IS_DIRECTORY "${_candidate}")
        list(APPEND _clap_binaries "${_candidate}")
    endif()
endforeach()
list(LENGTH _clap_binaries _clap_count)
if(NOT _clap_count EQUAL 1)
    message(FATAL_ERROR "expected one built .clap bundle, found: ${_clap_binaries}")
endif()

# The consumer configured without PULP_DEV_BUILD, so its image carries the
# embedded script and no on-disk path: the script's console marker proves the
# scan reads the right bytes, and the absolute source path must be absent.
# The image is named after PLUGIN_NAME (the CLAP target's OUTPUT_NAME), as the
# bundle is, not after the CMake target, and that name contains spaces.
file(GLOB_RECURSE _clap_images "${_clap_binaries}/*")
get_filename_component(_clap_image_name "${_clap_binaries}" NAME_WLE)
set(_clap_image "")
foreach(_candidate IN LISTS _clap_images)
    get_filename_component(_name "${_candidate}" NAME)
    if(_name STREQUAL _clap_image_name OR _name MATCHES "\\.(so|dll|clap)$")
        set(_clap_image "${_candidate}")
    endif()
endforeach()
if(NOT _clap_image)
    message(FATAL_ERROR "no plug-in image found inside ${_clap_binaries}: ${_clap_images}")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
function(_image_contains image needle out_var)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I -c
            "import sys, pathlib; sys.exit(0 if sys.argv[2].encode() in pathlib.Path(sys.argv[1]).read_bytes() else 1)"
            "${image}" "${needle}"
        RESULT_VARIABLE _rv)
    if(_rv EQUAL 0)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()
_image_contains("${_clap_image}" "pulp-ui-script-fixture-mounted" _has_marker)
_image_contains("${_clap_image}" "${_source}/ui/main.js" _has_path)
if(NOT _has_marker)
    message(FATAL_ERROR "${_clap_image} does not embed ui/main.js (the console marker is absent)")
endif()
if(_has_path)
    message(FATAL_ERROR
        "${_clap_image} names the on-disk script path ${_source}/ui/main.js in a release build")
endif()
message(STATUS "release image embeds the script and records no source path")

set(FORMAT clap)
set(BUNDLE "${_clap_binaries}")
include("${FIXTURE_DIR}/check_editor_mode.cmake")
