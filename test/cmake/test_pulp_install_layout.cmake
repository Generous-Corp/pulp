#[[
Install-layout regression for installed SDK CMake helper scripts
(issue-905 follow-up).

`pulp_add_binary_data` resolves its Python encoder via
${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — adjacent
to whichever `PulpUtils.cmake` is sourced. That convention only works in
the installed tree if the encoder is bundled alongside `PulpUtils.cmake`
under `lib/cmake/Pulp/scripts/`. If the install rule omits the encoder,
downstream `find_package(Pulp)` consumers see asset targets fail at
build time with a missing-script error.

This test runs `cmake --install` against a pre-configured Pulp build into a
temporary prefix, then asserts the bundled SDK contains `PulpUtils.cmake`,
helper modules it includes, and its sibling `scripts/encode_binary_data.py`.

Inputs (passed via -D):
  PULP_BUILD_DIR — path to a configured Pulp build directory.
]]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR
        "test_pulp_install_layout.cmake requires -DPULP_BUILD_DIR=<dir> "
        "pointing at a configured Pulp build.")
endif()
get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured CMake build "
        "directory (no CMakeCache.txt).")
endif()

set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-layout-test-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")

# `cmake --install` re-emits the install rules from the configured build.
# It does not re-invoke the build — only published outputs that were
# already produced by the previous `cmake --build` step are copied. The
# CMake-package files (PulpConfig.cmake, PulpUtils.cmake, …) are
# generated at configure time, so they are always present.
file(STRINGS "${PULP_BUILD_DIR}/CMakeCache.txt" _pulp_inspector_cache
    REGEX "^PULP_ENABLE_INSPECTOR:")
if(_pulp_inspector_cache MATCHES "=ON$")
    set(_pulp_inspector_mode on)
else()
    set(_pulp_inspector_mode off)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            --install "${PULP_BUILD_DIR}"
            --prefix  "${_prefix}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed (rc=${_rc}):\n"
        "----- stdout -----\n${_stdout}\n"
        "----- stderr -----\n${_stderr}\n----- end -----")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/scripts/doxygen_installed_header_check.py"
            --repo-root "${CMAKE_CURRENT_LIST_DIR}/../.."
            --installed-prefix "${_prefix}"
            --inspector-mode "${_pulp_inspector_mode}"
    RESULT_VARIABLE _header_parity_rc
    OUTPUT_VARIABLE _header_parity_stdout
    ERROR_VARIABLE _header_parity_stderr)
if(NOT _header_parity_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed public-header/Doxygen parity failed:\n"
        "${_header_parity_stdout}${_header_parity_stderr}")
endif()

# Installed CLI/MCP consumers must be able to locate the exact versioned
# machine contract without a Pulp checkout. Check both presence and bytes so a
# stale generated copy cannot silently diverge from the implementation corpus.
set(_installed_gpu_health_schema
    "${_prefix}/share/pulp/contracts/gpu-health-result-v1.schema.json")
if(NOT EXISTS "${_installed_gpu_health_schema}")
    message(FATAL_ERROR
        "GPU health result schema is missing from the installed SDK:\n"
        "${_installed_gpu_health_schema}")
endif()
set(_source_gpu_health_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-health-result-v1.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_health_schema}" "${_installed_gpu_health_schema}"
    RESULT_VARIABLE _gpu_health_schema_compare_rc)
if(NOT _gpu_health_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU health result schema differs from the source contract.")
endif()

set(_installed_gpu_health_v2_schema
    "${_prefix}/share/pulp/contracts/gpu-health-result-v2.schema.json")
if(NOT EXISTS "${_installed_gpu_health_v2_schema}")
    message(FATAL_ERROR
        "GPU health result v2 schema is missing from the installed SDK:\n"
        "${_installed_gpu_health_v2_schema}")
endif()
set(_source_gpu_health_v2_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-health-result-v2.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_health_v2_schema}" "${_installed_gpu_health_v2_schema}"
    RESULT_VARIABLE _gpu_health_v2_schema_compare_rc)
if(NOT _gpu_health_v2_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU health result v2 schema differs from the source contract.")
endif()

set(_installed_gpu_health_attestation_verification_schema
    "${_prefix}/share/pulp/contracts/gpu-health-run-attestation-verification-v1.schema.json")
if(NOT EXISTS "${_installed_gpu_health_attestation_verification_schema}")
    message(FATAL_ERROR
        "GPU health attestation verification schema is missing from the installed SDK:\n"
        "${_installed_gpu_health_attestation_verification_schema}")
endif()
set(_source_gpu_health_attestation_verification_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-health-run-attestation-verification-v1.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_health_attestation_verification_schema}"
            "${_installed_gpu_health_attestation_verification_schema}"
    RESULT_VARIABLE _gpu_health_attestation_verification_schema_compare_rc)
if(NOT _gpu_health_attestation_verification_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU health attestation verification schema differs from the source contract.")
endif()

foreach(_gpu_health_verifier_file IN ITEMS
        verify_gpu_health_run_attestation.py gpu_health_contract.py json_schema_lite.py)
    set(_installed_gpu_health_verifier_file
        "${_prefix}/share/pulp/gpu-health-run-attestation-verifier/${_gpu_health_verifier_file}")
    set(_source_gpu_health_verifier_file
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/scripts/${_gpu_health_verifier_file}")
    if(NOT EXISTS "${_installed_gpu_health_verifier_file}")
        message(FATAL_ERROR
            "GPU health attestation verifier member is missing from the installed SDK: "
            "${_installed_gpu_health_verifier_file}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source_gpu_health_verifier_file}"
                "${_installed_gpu_health_verifier_file}"
        RESULT_VARIABLE _gpu_health_verifier_file_compare_rc)
    if(NOT _gpu_health_verifier_file_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed GPU health attestation verifier member differs from source: "
            "${_gpu_health_verifier_file}")
    endif()
endforeach()
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${_prefix}/share/pulp/gpu-health-run-attestation-verifier/verify_gpu_health_run_attestation.py"
            --help
    RESULT_VARIABLE _gpu_health_verifier_help_rc
    OUTPUT_VARIABLE _gpu_health_verifier_help_stdout
    ERROR_VARIABLE _gpu_health_verifier_help_stderr)
if(NOT _gpu_health_verifier_help_rc EQUAL 0 OR
   NOT _gpu_health_verifier_help_stdout MATCHES "--attestation-revision" OR
   NOT _gpu_health_verifier_help_stdout MATCHES "--expected-producer-binary-path" OR
   _gpu_health_verifier_help_stdout MATCHES "--expected-producer-build-id" OR
   _gpu_health_verifier_help_stdout MATCHES "--expected-producer-code-signature")
    message(FATAL_ERROR
        "Installed GPU health attestation verifier is not runnable as a closed bundle:\n"
        "${_gpu_health_verifier_help_stdout}\n${_gpu_health_verifier_help_stderr}")
endif()

set(_installed_gpu_trace_human_review_schema
    "${_prefix}/share/pulp/contracts/gpu-trace-human-review-v1.schema.json")
if(NOT EXISTS "${_installed_gpu_trace_human_review_schema}")
    message(FATAL_ERROR
        "GPU trace human-review schema is missing from the installed SDK:\n"
        "${_installed_gpu_trace_human_review_schema}")
endif()
set(_source_gpu_trace_human_review_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-trace-human-review-v1.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_trace_human_review_schema}"
            "${_installed_gpu_trace_human_review_schema}"
    RESULT_VARIABLE _gpu_trace_human_review_schema_compare_rc)
if(NOT _gpu_trace_human_review_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU trace human-review schema differs from the source contract.")
endif()

set(_installed_gpu_probe_schema
    "${_prefix}/share/pulp/contracts/gpu-probe-result-v1.schema.json")
if(NOT EXISTS "${_installed_gpu_probe_schema}")
    message(FATAL_ERROR
        "GPU probe result schema is missing from the installed SDK:\n"
        "${_installed_gpu_probe_schema}")
endif()
set(_source_gpu_probe_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-probe-result-v1.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_probe_schema}" "${_installed_gpu_probe_schema}"
    RESULT_VARIABLE _gpu_probe_schema_compare_rc)
if(NOT _gpu_probe_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU probe result schema differs from the source contract.")
endif()

foreach(_gpu_recipe_catalog_name IN ITEMS gpu-recipes.yaml gpu-recipes.schema.json)
    set(_installed_gpu_recipe_catalog_file
        "${_prefix}/share/pulp/${_gpu_recipe_catalog_name}")
    set(_source_gpu_recipe_catalog_file
        "${CMAKE_CURRENT_LIST_DIR}/../../docs/status/${_gpu_recipe_catalog_name}")
    if(NOT EXISTS "${_installed_gpu_recipe_catalog_file}")
        message(FATAL_ERROR
            "GPU recipe catalog file is missing from the installed SDK:\n"
            "${_installed_gpu_recipe_catalog_file}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source_gpu_recipe_catalog_file}"
                "${_installed_gpu_recipe_catalog_file}"
        RESULT_VARIABLE _gpu_recipe_catalog_compare_rc)
    if(NOT _gpu_recipe_catalog_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed ${_gpu_recipe_catalog_name} differs from the source catalog.")
    endif()
endforeach()
unset(_gpu_recipe_catalog_compare_rc)
unset(_gpu_recipe_catalog_name)
unset(_installed_gpu_recipe_catalog_file)
unset(_source_gpu_recipe_catalog_file)

set(_installed_gpu_health_read_schema
    "${_prefix}/share/pulp/contracts/gpu-health-read-result-v1.schema.json")
if(NOT EXISTS "${_installed_gpu_health_read_schema}")
    message(FATAL_ERROR
        "GPU health-read result schema is missing from the installed SDK:\n"
        "${_installed_gpu_health_read_schema}")
endif()
set(_source_gpu_health_read_schema
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-health-read-result-v1.schema.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${_source_gpu_health_read_schema}"
            "${_installed_gpu_health_read_schema}"
    RESULT_VARIABLE _gpu_health_read_schema_compare_rc)
if(NOT _gpu_health_read_schema_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU health-read result schema differs from the source contract.")
endif()

foreach(_gpu_health_model_header IN ITEMS health_result.hpp health_read_result.hpp)
    set(_installed_gpu_health_model_header
        "${_prefix}/include/pulp_tooling/gpu_health/${_gpu_health_model_header}")
    set(_source_gpu_health_model_header
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/cli/gpu_health/include/pulp_tooling/gpu_health/${_gpu_health_model_header}")
    if(NOT EXISTS "${_installed_gpu_health_model_header}")
        message(FATAL_ERROR
            "GPU health model header is missing from the installed SDK:\n"
            "${_installed_gpu_health_model_header}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source_gpu_health_model_header}"
                "${_installed_gpu_health_model_header}"
        RESULT_VARIABLE _gpu_health_model_header_compare_rc)
    if(NOT _gpu_health_model_header_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed GPU health model header differs from source: "
            "${_gpu_health_model_header}")
    endif()
endforeach()
unset(_gpu_health_model_header)
unset(_installed_gpu_health_model_header)
unset(_source_gpu_health_model_header)
unset(_gpu_health_model_header_compare_rc)

foreach(_gpu_dpr_schema_version IN ITEMS v1 v2)
    set(_installed_gpu_dpr_schema
        "${_prefix}/share/pulp/contracts/gpu-dpr-experiment-${_gpu_dpr_schema_version}.schema.json")
    if(NOT EXISTS "${_installed_gpu_dpr_schema}")
        message(FATAL_ERROR
            "GPU DPR experiment schema is missing from the installed SDK:\n"
            "${_installed_gpu_dpr_schema}")
    endif()
    set(_source_gpu_dpr_schema
        "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/gpu-dpr-experiment-${_gpu_dpr_schema_version}.schema.json")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source_gpu_dpr_schema}" "${_installed_gpu_dpr_schema}"
        RESULT_VARIABLE _gpu_dpr_schema_compare_rc)
    if(NOT _gpu_dpr_schema_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed GPU DPR experiment schema differs from the source contract.")
    endif()
endforeach()
unset(_gpu_dpr_schema_version)

foreach(_gpu_dpr_v2_contract IN ITEMS
        gpu-dpr-corpus-v2-template.json
        gpu-dpr-live-verification-v1.schema.json
        gpu-vellum-package-terminal-v1.schema.json)
    set(_installed_gpu_dpr_v2_contract
        "${_prefix}/share/pulp/contracts/${_gpu_dpr_v2_contract}")
    set(_source_gpu_dpr_v2_contract
        "${CMAKE_CURRENT_LIST_DIR}/../../docs/contracts/${_gpu_dpr_v2_contract}")
    if(NOT EXISTS "${_installed_gpu_dpr_v2_contract}")
        message(FATAL_ERROR
            "GPU DPR v2 authority contract is missing from the installed SDK: "
            "${_installed_gpu_dpr_v2_contract}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_source_gpu_dpr_v2_contract}" "${_installed_gpu_dpr_v2_contract}"
        RESULT_VARIABLE _gpu_dpr_v2_contract_compare_rc)
    if(NOT _gpu_dpr_v2_contract_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed GPU DPR v2 authority contract differs from source: "
            "${_gpu_dpr_v2_contract}")
    endif()
endforeach()
unset(_gpu_dpr_v2_contract)
unset(_installed_gpu_dpr_v2_contract)
unset(_source_gpu_dpr_v2_contract)
unset(_gpu_dpr_v2_contract_compare_rc)

set(_gpu_health_front_args
    --prefix "${_prefix}")
if(PULP_EXPECT_GPU_CLI)
    list(APPEND _gpu_health_front_args --require-cpp)
endif()
if(PULP_EXPECT_RUST_CLI)
    list(APPEND _gpu_health_front_args --require-rust)
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../../tools/scripts/test_gpu_health_installed_fronts.py"
            ${_gpu_health_front_args}
    RESULT_VARIABLE _gpu_health_fronts_rc
    OUTPUT_VARIABLE _gpu_health_fronts_stdout
    ERROR_VARIABLE _gpu_health_fronts_stderr)
if(NOT _gpu_health_fronts_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed GPU health CLI/MCP fronts failed:\n"
        "${_gpu_health_fronts_stdout}${_gpu_health_fronts_stderr}")
endif()
unset(_gpu_health_front_args)

# Locate the installed PulpUtils.cmake — Pulp installs into
# ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp, which is libdir/cmake/Pulp on
# every platform. libdir varies (lib vs lib64); glob both.
file(GLOB _pulp_utils LIST_DIRECTORIES false
    "${_prefix}/lib*/cmake/Pulp/PulpUtils.cmake")
if(NOT _pulp_utils)
    message(FATAL_ERROR
        "Installed PulpUtils.cmake not found under ${_prefix}/lib*/cmake/Pulp/.")
endif()
list(GET _pulp_utils 0 _pulp_utils)
get_filename_component(_pulp_cmake_dir "${_pulp_utils}" DIRECTORY)

set(_installed_metadata "${_pulp_cmake_dir}/PulpPluginMetadata.cmake")
if(NOT EXISTS "${_installed_metadata}")
    message(FATAL_ERROR
        "PulpPluginMetadata.cmake not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_metadata}\n"
        "PulpUtils.cmake includes this helper, so find_package(Pulp) consumers "
        "would fail before they can call pulp_add_plugin().")
endif()
file(READ "${_pulp_utils}" _installed_utils_text)
if(NOT _installed_utils_text MATCHES "PulpPluginMetadata\\.cmake")
    message(FATAL_ERROR
        "Installed PulpUtils.cmake no longer includes PulpPluginMetadata.cmake.")
endif()

# The encoder MUST live next to PulpUtils.cmake — this is the path
# pulp_add_binary_data computes via CMAKE_CURRENT_FUNCTION_LIST_DIR.
set(_installed_encoder "${_pulp_cmake_dir}/scripts/encode_binary_data.py")
if(NOT EXISTS "${_installed_encoder}")
    message(FATAL_ERROR
        "Encoder script not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_encoder}\n"
        "find_package(Pulp) consumers calling pulp_add_binary_data would "
        "fail at build time with a missing-script error. The install rule "
        "in CMakeLists.txt must publish "
        "tools/cmake/scripts/encode_binary_data.py under "
        "lib/cmake/Pulp/scripts/.")
endif()

# Sanity: the script we shipped must be the same one we built against.
file(SIZE "${_installed_encoder}" _installed_size)
get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_source_encoder "${_repo_root}/tools/cmake/scripts/encode_binary_data.py")
if(NOT EXISTS "${_source_encoder}")
    message(FATAL_ERROR
        "Source encoder ${_source_encoder} not found — repo layout broken?")
endif()
file(SIZE "${_source_encoder}" _source_size)
if(NOT _installed_size EQUAL _source_size)
    message(FATAL_ERROR
        "Installed encoder size (${_installed_size}) differs from "
        "source encoder size (${_source_size}) — install rule may be "
        "publishing a stale or different file.")
endif()

# Min-OS floor propagation: the SDK must ship PulpMinOs.cmake AND min_os.json
# beside it, and PulpConfig.cmake must include PulpMinOs.cmake. Without all
# three, a find_package(Pulp) consumer cannot resolve Pulp's floor and its
# plugin silently inherits the build host's OS floor (e.g. a macOS-26 machine
# ships a macOS-26-only plugin instead of Pulp's 13.4).
set(_installed_minos "${_pulp_cmake_dir}/PulpMinOs.cmake")
if(NOT EXISTS "${_installed_minos}")
    message(FATAL_ERROR
        "PulpMinOs.cmake not bundled with the installed Pulp SDK.\n"
        "Expected: ${_installed_minos}\n"
        "Consumers would not pin Pulp's min-OS floor and would ship plugins "
        "targeting the build host's OS version.")
endif()
set(_installed_minos_json "${_pulp_cmake_dir}/min_os.json")
if(NOT EXISTS "${_installed_minos_json}")
    message(FATAL_ERROR
        "min_os.json not bundled next to PulpMinOs.cmake in the installed SDK.\n"
        "Expected: ${_installed_minos_json}\n"
        "PulpMinOs.cmake finds no floor data and cannot pin the consumer.")
endif()
file(GLOB _pulp_config LIST_DIRECTORIES false
    "${_pulp_cmake_dir}/PulpConfig.cmake")
if(NOT _pulp_config)
    message(FATAL_ERROR
        "Installed PulpConfig.cmake not found under ${_pulp_cmake_dir}.")
endif()
list(GET _pulp_config 0 _pulp_config)
file(READ "${_pulp_config}" _installed_config_text)
if(NOT _installed_config_text MATCHES "PulpMinOs\\.cmake")
    message(FATAL_ERROR
        "Installed PulpConfig.cmake does not include PulpMinOs.cmake — the "
        "min-OS floor would not be pinned for find_package(Pulp) consumers.")
endif()

# A configured Three.js runtime is part of the installed SDK, not a source-tree
# convenience. Prove the complete resolver payload exists, then configure a
# consumer in a separate directory and require find_package(Pulp) to resolve the
# runtime back into this exact installation prefix.
file(STRINGS "${PULP_BUILD_DIR}/CMakeCache.txt" _threejs_runtime_cache
    REGEX "^PULP_ENABLE_THREEJS_RUNTIME:BOOL=")
if(_threejs_runtime_cache MATCHES "=ON$")
    set(_threejs_runtime_root "${_prefix}/share/pulp/threejs")
    set(_threejs_bundler
        "${_pulp_cmake_dir}/scripts/bundle_threejs_for_jsc.mjs")
    set(_threejs_shim
        "${_pulp_cmake_dir}/scripts/web-compat-three-shim.js")
    foreach(_threejs_build_asset IN ITEMS "${_threejs_bundler}" "${_threejs_shim}")
        if(NOT EXISTS "${_threejs_build_asset}")
            message(FATAL_ERROR
                "Installed Three.js AUv3 build asset is missing: ${_threejs_build_asset}")
        endif()
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/threejs-runtime-manifest.json"
            "${_threejs_runtime_root}/threejs-runtime-manifest.json"
        RESULT_VARIABLE _threejs_manifest_compare_rc)
    if(NOT _threejs_manifest_compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed Three.js runtime manifest differs from the pinned source manifest.")
    endif()

    set(_threejs_consumer_source
        "${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-installed-consumer")
    set(_threejs_consumer_build
        "${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-installed-consumer-build")
    file(REMOVE_RECURSE "${_threejs_consumer_source}" "${_threejs_consumer_build}")
    file(MAKE_DIRECTORY "${_threejs_consumer_source}")
    file(WRITE "${_threejs_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpThreeJsInstalledConsumer LANGUAGES CXX)
find_package(Pulp CONFIG REQUIRED)
if(NOT PULP_HAS_THREEJS_RUNTIME)
    message(FATAL_ERROR "Installed Pulp package did not advertise Three.js runtime")
endif()
file(REAL_PATH "${PULP_THREEJS_RUNTIME_DIR}" _runtime_real)
file(REAL_PATH "${EXPECTED_RUNTIME_DIR}" _expected_real)
if(NOT _runtime_real STREQUAL _expected_real)
    message(FATAL_ERROR
        "Three.js runtime resolved outside selected SDK: "
        "${_runtime_real} != ${_expected_real}")
endif()
if(NOT PULP_THREEJS_RUNTIME_REVISION STREQUAL "${EXPECTED_THREEJS_REVISION}")
    message(FATAL_ERROR
        "Unexpected Three.js revision: ${PULP_THREEJS_RUNTIME_REVISION}")
endif()
foreach(_asset_var IN ITEMS PULP_THREEJS_BUNDLER_SCRIPT PULP_THREEJS_WEB_COMPAT_SHIM)
    if(NOT EXISTS "${${_asset_var}}")
        message(FATAL_ERROR "Installed Three.js AUv3 asset did not resolve: ${_asset_var}")
    endif()
endforeach()
]=])
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${_threejs_consumer_source}"
            -B "${_threejs_consumer_build}"
            "-DCMAKE_PREFIX_PATH=${_prefix}"
            -DPULP_ALLOW_DEBUG_SDK=ON
            "-DEXPECTED_RUNTIME_DIR=${_threejs_runtime_root}"
            "-DEXPECTED_THREEJS_REVISION=077dd13c0e869d9f3dbe55875686f920367de457"
        RESULT_VARIABLE _threejs_consumer_rc
        OUTPUT_VARIABLE _threejs_consumer_stdout
        ERROR_VARIABLE _threejs_consumer_stderr)
    if(NOT _threejs_consumer_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed Three.js consumer configure failed:\n"
            "${_threejs_consumer_stdout}${_threejs_consumer_stderr}")
    endif()

    # On Apple hosts, configure the public iOS AUv3 helper from the installed
    # package and prove its generated build rule references only installed,
    # verified Three.js inputs plus the packaged bundler/shim. Configure-only is
    # intentional: this host SDK's archives are macOS artifacts, while the
    # boundary under test is downstream helper generation and asset routing.
    if(CMAKE_HOST_APPLE)
        find_program(_threejs_xcrun NAMES xcrun)
        if(_threejs_xcrun)
            execute_process(
                COMMAND "${_threejs_xcrun}" --sdk iphonesimulator --show-sdk-path
                RESULT_VARIABLE _threejs_ios_sdk_rc
                OUTPUT_QUIET ERROR_QUIET)
        endif()
        if(_threejs_xcrun AND _threejs_ios_sdk_rc EQUAL 0)
            set(_threejs_ios_consumer_source
                "${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-ios-installed-consumer")
            set(_threejs_ios_consumer_build
                "${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-ios-installed-consumer-build")
            file(REMOVE_RECURSE
                "${_threejs_ios_consumer_source}" "${_threejs_ios_consumer_build}")
            file(MAKE_DIRECTORY "${_threejs_ios_consumer_source}")
            file(WRITE "${_threejs_ios_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpThreeJsIosInstalledConsumer LANGUAGES C CXX OBJC OBJCXX)
# This configure-only helper proof consumes a macOS SDK install while selecting
# the iOS toolchain. Its mac-gpu Skia archive is intentionally not an iOS slice;
# provide only the imported target name needed to validate generated build rules.
add_library(skia::skia INTERFACE IMPORTED)
find_package(Pulp CONFIG REQUIRED)
pulp_add_ios_auv3(
    NAME InstalledThreeJs
    BUNDLE_ID com.pulp.tests.host.InstalledThreeJs
    MANUFACTURER Pulp
    MANUFACTURER_CODE Pulp
    SUBTYPE_CODE ItJs
    AU_TYPE aumu
    VERSION 1.0.0)
]=])
            execute_process(
                COMMAND "${CMAKE_COMMAND}"
                    -S "${_threejs_ios_consumer_source}"
                    -B "${_threejs_ios_consumer_build}"
                    -G Xcode
                    -DCMAKE_SYSTEM_NAME=iOS
                    -DCMAKE_OSX_SYSROOT=iphonesimulator
                    -DCMAKE_OSX_ARCHITECTURES=arm64
                    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4
                    "-DCMAKE_PREFIX_PATH=${_prefix}"
                    "-DPulp_DIR=${_pulp_cmake_dir}"
                    -DPULP_ALLOW_DEBUG_SDK=ON
                    "-D_PULP_NODE_EXE=${CMAKE_COMMAND}"
                RESULT_VARIABLE _threejs_ios_consumer_rc
                OUTPUT_VARIABLE _threejs_ios_consumer_stdout
                ERROR_VARIABLE _threejs_ios_consumer_stderr)
            if(NOT _threejs_ios_consumer_rc EQUAL 0)
                message(FATAL_ERROR
                    "Installed Three.js iOS AUv3 consumer configure failed:\n"
                    "${_threejs_ios_consumer_stdout}${_threejs_ios_consumer_stderr}")
            endif()
            file(READ
                "${_threejs_ios_consumer_build}/PulpThreeJsIosInstalledConsumer.xcodeproj/project.pbxproj"
                _threejs_ios_project)
            foreach(_threejs_expected_build_input IN ITEMS
                    "${_threejs_runtime_root}/build/three.webgpu.js"
                    "${_threejs_runtime_root}/examples/jsm/controls/OrbitControls.js"
                    "${_threejs_bundler}"
                    "${_threejs_shim}")
                string(FIND "${_threejs_ios_project}"
                    "${_threejs_expected_build_input}" _threejs_input_offset)
                if(_threejs_input_offset EQUAL -1)
                    message(FATAL_ERROR
                        "Installed iOS AUv3 rule omitted Three.js input: "
                        "${_threejs_expected_build_input}")
                endif()
            endforeach()
        endif()
    endif()
endif()

# Pulp's installed Windows archives are built with the static MSVC runtime.
# find_package(Pulp) must establish the same default before a consumer creates
# targets, or their /MD objects fail to link against Pulp's /MT archives with
# LNK2038. Keep both the override guard and the Release archive's exact value
# in the installed package contract.
if(NOT _installed_config_text MATCHES
        "MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY")
    message(FATAL_ERROR
        "Installed PulpConfig.cmake does not preserve a consumer-selected "
        "MSVC runtime before applying Pulp's compatible default.")
endif()
if(NOT _installed_config_text MATCHES
        "set\\(CMAKE_MSVC_RUNTIME_LIBRARY[
 ]+\"MultiThreaded\"")
    message(FATAL_ERROR
        "Installed PulpConfig.cmake does not default every consumer "
        "configuration to the Release static MSVC runtime used by Pulp's "
        "Release-only installed archives.")
endif()

# pulp::osc export: the OSC sender/receiver is a public SDK subsystem, but was
# long absent from PULP_SDK_TARGETS and the header-install loop. A consumer that
# links pulp::osc (a CV-to-OSC plugin, say) then fails find_package(Pulp) with an
# unknown target, or fails to compile against a missing pulp/osc header. Assert
# both halves of the export: the public header, and the exported target.
if(NOT EXISTS "${_prefix}/include/pulp/osc/osc.hpp")
    message(FATAL_ERROR
        "pulp/osc/osc.hpp not installed under ${_prefix}/include/pulp/osc/.\n"
        "The osc subsystem is missing from the header-install loop in "
        "PulpInstallRules.cmake, so find_package(Pulp) consumers that include "
        "<pulp/osc/osc.hpp> would fail to compile.")
endif()
# The install exports the imported target under the `Pulp::` namespace in
# PulpTargets*.cmake; PulpConfig.cmake then adds the lower-case `pulp::osc`
# alias consumers actually link. Assert both halves so neither can silently
# drop: the raw `Pulp::osc` export AND `pulp-osc`'s membership in the
# lower-case alias loop's target list.
file(GLOB _pulp_targets LIST_DIRECTORIES false "${_pulp_cmake_dir}/PulpTargets*.cmake")
set(_osc_target_found FALSE)
foreach(_tf IN LISTS _pulp_targets)
    file(READ "${_tf}" _tf_text)
    if(_tf_text MATCHES "Pulp::osc")
        set(_osc_target_found TRUE)
    endif()
endforeach()
if(NOT _osc_target_found)
    message(FATAL_ERROR
        "Pulp::osc is not in the exported target set (PulpTargets*.cmake under "
        "${_pulp_cmake_dir}).\n"
        "Add pulp-osc to PULP_SDK_TARGETS in PulpInstallRules.cmake so "
        "find_package(Pulp) consumers can link pulp::osc.")
endif()

# Plugin control is exposed through the capability-controlled service rather
# than the retired format-local Remote View session. Keep both the old header
# and its entry points out of the installed SDK so downstream code cannot
# silently recreate the parallel authority path.
set(_remote_view_header
    "${_prefix}/include/pulp/format/remote_view_session.hpp")
if(EXISTS "${_remote_view_header}")
    message(FATAL_ERROR
        "Retired Remote View session header leaked into the installed SDK: "
        "${_remote_view_header}")
endif()
set(_installed_view_bridge
    "${_prefix}/include/pulp/format/view_bridge.hpp")
if(NOT EXISTS "${_installed_view_bridge}")
    message(FATAL_ERROR
        "Installed ViewBridge header is missing: ${_installed_view_bridge}")
endif()
file(READ "${_installed_view_bridge}" _installed_view_bridge_text)
foreach(_retired_remote_view_symbol IN ITEMS
        "RemoteViewSession" "attach_remote_channel" "detach_remote"
        "ViewRole::Remote")
    string(FIND "${_installed_view_bridge_text}"
        "${_retired_remote_view_symbol}" _retired_symbol_index)
    if(NOT _retired_symbol_index EQUAL -1)
        message(FATAL_ERROR
            "Installed ViewBridge exposes retired Remote View entry point: "
            "${_retired_remote_view_symbol}")
    endif()
endforeach()

file(READ "${_pulp_config}" _installed_config_alias_text)
if(NOT _installed_config_alias_text MATCHES "pulp-osc")
    message(FATAL_ERROR
        "pulp-osc is not in the lower-case alias loop's export-target list in "
        "PulpConfig.cmake, so the `pulp::osc` alias consumers link would never "
        "be created.\n"
        "Ensure pulp-osc is in PULP_SDK_TARGETS in PulpInstallRules.cmake.")
endif()

# The optional Ableton Link adapter is source-build-only. Its declaration must
# not be swept into the installed playback headers, and neither its target nor
# the developer's SDK path may leak into the exported CMake package.
if(EXISTS "${_prefix}/include/pulp/playback/ableton_link.hpp")
    message(FATAL_ERROR
        "Source-build-only ableton_link.hpp leaked into the installed SDK at "
        "${_prefix}/include/pulp/playback/ableton_link.hpp.")
endif()
set(_pulp_export_text "")
foreach(_tf IN LISTS _pulp_targets)
    file(READ "${_tf}" _tf_text)
    string(APPEND _pulp_export_text "\n${_tf_text}")
endforeach()
string(APPEND _pulp_export_text "\n${_installed_config_alias_text}")
if(_pulp_export_text MATCHES "ableton[-_]link|Ableton::Link")
    message(FATAL_ERROR
        "The source-build-only Ableton Link adapter leaked into the installed "
        "Pulp CMake target/config closure under ${_pulp_cmake_dir}.")
endif()
file(STRINGS "${PULP_BUILD_DIR}/CMakeCache.txt" _link_sdk_cache_line
    REGEX "^PULP_ABLETON_LINK_SDK_DIR(:[^=]*)?=")
foreach(_line IN LISTS _link_sdk_cache_line)
    string(REGEX REPLACE "^[^=]*=" "" _link_sdk_path "${_line}")
    string(FIND "${_pulp_export_text}" "${_link_sdk_path}" _link_sdk_path_index)
    if(NOT _link_sdk_path STREQUAL "" AND NOT _link_sdk_path_index EQUAL -1)
        message(FATAL_ERROR
            "Developer-supplied Ableton Link SDK path leaked into the installed "
            "Pulp CMake package: ${_link_sdk_path}")
    endif()
endforeach()

message(STATUS
    "Install layout: PulpUtils.cmake at ${_pulp_utils}, encoder at "
    "${_installed_encoder} (${_installed_size} bytes), PulpMinOs.cmake + "
    "min_os.json bundled and wired into PulpConfig.cmake, pulp::osc header + "
    "target exported; GPU health contract and configured Three.js runtime "
    "installed outside the source tree; Ableton Link "
    "header, target, and SDK path absent. All present.")
