cmake_minimum_required(VERSION 3.24)

# Behavioral proof that an INSTALLED Pulp SDK stages its runtime sidecars into
# real plug-in/app outputs (WAH-3).
#
# What this replaces: test_windows_skia_runtime_sidecar.cmake, which `file(READ)`
# the text of PulpWebGpuImportedTarget.cmake and asserted four regexes matched.
# That proves somebody wrote a copy rule. It does not prove:
#
#   * the installed SDK exposes the ICU data file at all,
#   * the staging helper is invoked for any given format's target,
#   * anything actually lands in the built bundle.
#
# Every one of those fails silently until a user's DAW crashes on the plug-in's
# first text render — SkParagraph's ICU-backed SkUnicode returns null without
# `icudtl.dat` and deliberately traps on its `fUnicode` check.
#
# So this installs the SDK, configures and builds the sdk-smoke consumer against
# it with Standalone + VST3 + CLAP required, and lets the SDK's own
# pulp_verify_runtime_dependencies_staged() POST_BUILD check assert the files
# exist beside each module. A missing sidecar fails the BUILD, which is what
# makes this a real gate rather than a description of one.
#
# Platform note: `icudtl.dat` is a Windows-only sidecar, so on macOS/Linux this
# verifies the wgpu-native runtime instead. That is deliberate — a test that
# only ran under `_WIN32` would never execute on Pulp's required macOS gate, and
# the invocation plumbing it covers (helper exported, script installed beside
# it, every format target wired) is exactly the part that breaks
# platform-independently.

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/sdk-runtime-staging-smoke")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_config}" _config_lower)

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "SDK staging failed (${_install_result})\n${_install_output}\n${_install_error}")
endif()

# The verifier script must ride along with the module that resolves it, or a
# consumer can stage sidecars but never check them.
if(NOT EXISTS "${_prefix}/lib/cmake/Pulp/PulpVerifyRuntimeStaging.cmake")
    message(FATAL_ERROR
        "Installed SDK is missing lib/cmake/Pulp/PulpVerifyRuntimeStaging.cmake. "
        "PulpWebGpuImportedTarget.cmake resolves it relative to its own "
        "directory, so an SDK without it cannot verify its own bundles.")
endif()

set(_configure_args
    -S "${PULP_SOURCE_DIR}/tools/validation/sdk-smoke"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_config}")

# Match the sibling installed-SDK fixtures: a Debug producer (the sanitizer
# lanes) is an acknowledged configuration, not a failure.
if(_config_lower STREQUAL "debug")
    list(APPEND _configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Runtime-staging consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

# The consumer's own POST_BUILD verification runs here. A module whose sidecars
# did not land fails the build with the file list it looked for.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}" --config "${_config}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Runtime-staging consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

# The POST_BUILD check is a no-op when nothing is required, so assert
# independently that it actually RAN for at least one target. Otherwise a
# regression that silently stopped wiring the verification would look identical
# to a clean pass.
if(NOT _build_output MATCHES "pulp-runtime-staging:")
    message(FATAL_ERROR
        "The consumer build produced no pulp-runtime-staging output, so the "
        "verification never ran for any format target. Check that "
        "tools/validation/sdk-smoke wires "
        "pulp_verify_runtime_dependencies_staged() for every built format.\n"
        "${_build_output}")
endif()

message(STATUS "installed_sdk_runtime_staging_verified=true")
