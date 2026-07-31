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

# The SDK's design-import frontend delegates to this installed helper. Launch
# it from the staged prefix so a missing install rpath fails here instead of
# later in Forge (or another SDK consumer) with a dynamic-loader error. Windows
# has no rpath and its SDK DLL layout is a separate packaging contract.
if(NOT WIN32)
    set(_import_design_helper "${_prefix}/bin/pulp-import-design")
    if(NOT EXISTS "${_import_design_helper}")
        message(FATAL_ERROR
            "Installed SDK is missing ${_import_design_helper}")
    endif()
    execute_process(
        COMMAND "${_import_design_helper}" --help
        RESULT_VARIABLE _import_design_result
        OUTPUT_VARIABLE _import_design_output
        ERROR_VARIABLE _import_design_error)
    if(NOT _import_design_result EQUAL 0)
        message(FATAL_ERROR
            "Installed pulp-import-design failed to launch (${_import_design_result}). "
            "Verify that its install rpath resolves shared SDK libraries.\n"
            "${_import_design_output}\n${_import_design_error}")
    endif()
endif()

set(_configure_args
    -S "${PULP_SOURCE_DIR}/tools/validation/sdk-smoke"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_config}")

# An installed SDK produced under a sanitizer contains instrumented static
# libraries. Its smoke consumer must use the same compile/link flags or the
# first executable fails to resolve the sanitizer runtime before the staging
# behavior can run.
foreach(_flag_var CXX_FLAGS C_FLAGS OBJCXX_FLAGS EXE_LINKER_FLAGS SHARED_LINKER_FLAGS)
    if(DEFINED PULP_PARENT_${_flag_var} AND NOT PULP_PARENT_${_flag_var} STREQUAL "")
        list(APPEND _configure_args
            "-DCMAKE_${_flag_var}=${PULP_PARENT_${_flag_var}}")
    endif()
endforeach()

# The relocatability checker deliberately keeps compiler sanitizer runtimes
# strict for shipping bundles and softens them only for explicitly marked
# test-only sanitizer builds. Raw CMAKE_*_FLAGS carry the instrumentation into
# this nested consumer, but not that identity, so propagate it when (and only
# when) the inherited flags actually request a sanitizer.
set(_parent_instrumentation_flags
    "${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_C_FLAGS} "
    "${PULP_PARENT_OBJCXX_FLAGS} ${PULP_PARENT_EXE_LINKER_FLAGS} "
    "${PULP_PARENT_SHARED_LINKER_FLAGS}")
if(_parent_instrumentation_flags MATCHES "(^|[ ;])-fsanitize=([^ ;]+)")
    list(APPEND _configure_args "-DPULP_SANITIZER=${CMAKE_MATCH_2}")
endif()

# Match the sibling installed-SDK fixtures: a Debug producer (the sanitizer
# lanes) is an acknowledged configuration, not a failure.
if(_config_lower STREQUAL "debug")
    list(APPEND _configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()

# The SDK under test comes from the parent build. When that producer is
# instrumented, its installed static libraries retain references to the
# matching runtime. Propagate the producer flags into this real external
# consumer just as the Timeline SDK fixture does; otherwise the proof fails at
# link time before it can exercise runtime-sidecar staging.
set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags
        " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags
        " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
string(STRIP "${_consumer_linker_flags}" _consumer_linker_flags)

if(PULP_PARENT_SANITIZER)
    list(APPEND _configure_args
        "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _configure_args
        "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(_consumer_linker_flags)
    list(APPEND _configure_args
        "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")
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
