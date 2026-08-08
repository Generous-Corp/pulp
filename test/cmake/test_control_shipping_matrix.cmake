if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
set(_installed_helper_dir "${FIXTURE_DIR}/installed/lib/cmake/Pulp")
file(MAKE_DIRECTORY "${_installed_helper_dir}")
file(COPY
    "${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake"
    "${PULP_SOURCE_DIR}/tools/cmake/PulpControlShipping.cmake"
    "${PULP_SOURCE_DIR}/tools/cmake/check_control_shipping_artifact.cmake"
    DESTINATION "${_installed_helper_dir}")

set(_matrix_source "${FIXTURE_DIR}/matrix-source")
set(_matrix_build "${FIXTURE_DIR}/matrix-build")
file(MAKE_DIRECTORY "${_matrix_source}")
file(WRITE "${_matrix_source}/main.cpp"
    "int main() { return 0; }\n")
file(WRITE "${_matrix_source}/module.cpp"
    "extern \"C\" int pulp_control_shipping_fixture() { return 0; }\n")
file(WRITE "${_matrix_source}/runtime_eval.cpp"
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char runtime_eval_component[] = \"PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1\";\n")

file(WRITE "${_matrix_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(ControlShippingMatrix LANGUAGES CXX)
include("@HELPER@/PulpControlShipping.cmake")

function(add_shipping_artifact target profile capabilities format)
    set(_eval_ack FALSE)
    if(profile STREQUAL "research-unsafe")
        set(_eval_ack TRUE)
    endif()
    _pulp_cache_control_declarations(${target} "${profile}" "${capabilities}" "${_eval_ack}")
    _pulp_configure_control_shipping(${target} "com.pulp.test.${target}" "${target}")
    if(format STREQUAL "Standalone")
        add_executable(${target} main.cpp)
    else()
        add_library(${target} MODULE module.cpp)
    endif()
    if(NOT profile STREQUAL "production-stripped" AND format STREQUAL "Standalone")
        # This source stands in for the linked control implementation. The
        # shipping helper must not manufacture these implementation markers.
        set(_implementation_markers "PULP_INSPECT_SHIPPING_MANIFEST_V1")
        foreach(_capability IN LISTS capabilities)
            list(FIND _PULP_CONTROL_CAPABILITIES "${_capability}" _capability_index)
            list(GET _PULP_INSPECTOR_SHIPPING_CAPABILITIES
                ${_capability_index} _legacy_capability)
            string(MAKE_C_IDENTIFIER "${_legacy_capability}" _capability_identifier)
            string(TOUPPER "${_capability_identifier}" _capability_identifier)
            string(APPEND _implementation_markers
                "\\0PULP_INSPECT_CAPABILITY_${_capability_identifier}_V1")
        endforeach()
        set(_implementation_source
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_control_implementation.cpp")
        file(GENERATE OUTPUT "${_implementation_source}" CONTENT
            "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char ${target}_control_implementation[] = \"${_implementation_markers}\";\n")
        target_sources(${target} PRIVATE "${_implementation_source}")
    endif()
    if(profile STREQUAL "research-unsafe")
        target_sources(${target} PRIVATE runtime_eval.cpp)
    endif()
    _pulp_attach_control_shipping(${target} ${target} ${format})
endfunction()

add_shipping_artifact(ProductionStandalone production-stripped "" Standalone)
add_shipping_artifact(ProductionVST3 production-stripped "" VST3)
add_shipping_artifact(ProductionCLAP production-stripped "" CLAP)
add_shipping_artifact(ProductionAU production-stripped "" AUv2)
add_shipping_artifact(ProductionAUv3 production-stripped "" AUv3Extension)
add_shipping_artifact(ProductionAAX production-stripped "" AAX)
add_shipping_artifact(ProductionLV2 production-stripped "" LV2)
add_shipping_artifact(DeveloperStandalone developer-local
    "dev.pulp.instance/read@1;dev.pulp.state/read@1" Standalone)
add_shipping_artifact(DeveloperVST3 developer-local
    "dev.pulp.instance/read@1" VST3)
add_shipping_artifact(TestStandalone test-deterministic
    "dev.pulp.instance/read@1" Standalone)
add_shipping_artifact(SupportStandalone support-diagnostics
    "dev.pulp.diagnostics/read@1;dev.pulp.logs/read@1" Standalone)
add_shipping_artifact(ResearchStandalone research-unsafe
    "dev.pulp.session/control@1;dev.pulp.runtime/evaluate@1" Standalone)
]=])
file(READ "${_matrix_source}/CMakeLists.txt" _matrix_cmake)
string(REPLACE "@HELPER@" "${_installed_helper_dir}" _matrix_cmake "${_matrix_cmake}")
file(WRITE "${_matrix_source}/CMakeLists.txt" "${_matrix_cmake}")

set(_matrix_architecture_args "")
if(APPLE)
    find_program(_matrix_lipo NAMES lipo)
    if(_matrix_lipo)
        list(APPEND _matrix_architecture_args
            "-DCMAKE_OSX_ARCHITECTURES=arm64\\;x86_64")
    endif()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_matrix_source}" -B "${_matrix_build}"
    -DCMAKE_BUILD_TYPE=Release ${_matrix_architecture_args}
    RESULT_VARIABLE _configure_result OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "could not configure control shipping matrix: ${_configure_output}${_configure_error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_matrix_build}" --config Release --parallel 4
    RESULT_VARIABLE _build_result OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "control shipping matrix failed: ${_build_output}${_build_error}")
endif()

file(GLOB _reports "${_matrix_build}/*control-shipping-report.json")
list(LENGTH _reports _report_count)
if(NOT _report_count EQUAL 12)
    message(FATAL_ERROR
        "control shipping matrix emitted ${_report_count} reports instead of 12")
endif()
foreach(_report IN LISTS _reports)
    file(READ "${_report}" _report_content)
    foreach(_required IN ITEMS
            "\"artifact_size_bytes\":" "\"architectures\":"
            "\"symbol_scanner\":" "\"dependency_scanner\":"
            "\"architecture_scanner\":"
            "\"packaged_dependency_binaries_scanned\":"
            "\"verdict\": \"pass\"")
        string(FIND "${_report_content}" "${_required}" _required_position)
        if(_required_position LESS 0)
            message(FATAL_ERROR "shipping report is incomplete: ${_report}")
        endif()
    endforeach()
endforeach()

set(_production_report
    "${_matrix_build}/ProductionStandalone.Standalone.control-shipping-report.json")
if(NOT EXISTS "${_production_report}")
    message(FATAL_ERROR "production scan report is missing")
endif()
file(TIMESTAMP "${_production_report}" _report_before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(TOUCH
    "${_installed_helper_dir}/check_control_shipping_artifact.cmake")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_matrix_build}" --config Release
    --target ProductionStandalone_ControlShippingScan_STANDALONE
    RESULT_VARIABLE _rescan_result OUTPUT_VARIABLE _rescan_output
    ERROR_VARIABLE _rescan_error)
if(NOT _rescan_result EQUAL 0)
    message(FATAL_ERROR
        "persisted shipping rescan failed: ${_rescan_output}${_rescan_error}")
endif()
file(TIMESTAMP "${_production_report}" _report_after "%s")
if(NOT _report_after GREATER _report_before)
    message(FATAL_ERROR
        "scanner policy change did not invalidate the persisted report")
endif()

# An intentional declaration without a linked implementation must fail. This
# prevents the shipping metadata translation unit from becoming self-attested
# evidence for endpoint or capability linkage.
set(_missing_source "${FIXTURE_DIR}/missing-implementation-source")
set(_missing_build "${FIXTURE_DIR}/missing-implementation-build")
file(MAKE_DIRECTORY "${_missing_source}")
file(WRITE "${_missing_source}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${_missing_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(MissingControlImplementation LANGUAGES CXX)
include("@HELPER@/PulpControlShipping.cmake")
_pulp_cache_control_declarations(Missing developer-local
    "dev.pulp.instance/read@1" FALSE)
_pulp_configure_control_shipping(Missing "com.pulp.test.missing" "Missing")
add_executable(Missing main.cpp)
_pulp_attach_control_shipping(Missing Missing Standalone)
]=])
file(READ "${_missing_source}/CMakeLists.txt" _missing_cmake)
string(REPLACE "@HELPER@" "${_installed_helper_dir}" _missing_cmake "${_missing_cmake}")
file(WRITE "${_missing_source}/CMakeLists.txt" "${_missing_cmake}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_missing_source}" -B "${_missing_build}"
    -DCMAKE_BUILD_TYPE=Release RESULT_VARIABLE _missing_configure
    OUTPUT_QUIET ERROR_QUIET)
if(NOT _missing_configure EQUAL 0)
    message(FATAL_ERROR "could not configure missing implementation fixture")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_missing_build}" --config Release
    RESULT_VARIABLE _missing_build_result OUTPUT_VARIABLE _missing_output
    ERROR_VARIABLE _missing_error)
if(_missing_build_result EQUAL 0)
    message(FATAL_ERROR "control declaration without implementation was accepted")
endif()
string(FIND "${_missing_output}${_missing_error}"
    "control-enabled profile has no retained endpoint marker" _missing_reason)
if(_missing_reason LESS 0)
    message(FATAL_ERROR
        "missing implementation failed for the wrong reason: ${_missing_output}${_missing_error}")
endif()

function(expect_leak_blocked case_name leak_source)
    set(_source "${FIXTURE_DIR}/${case_name}-source")
    set(_build "${FIXTURE_DIR}/${case_name}-build")
    file(MAKE_DIRECTORY "${_source}")
    file(WRITE "${_source}/leak.cpp" "${leak_source}\nint main() { return 0; }\n")
    file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(ControlLeak LANGUAGES CXX)
include("@HELPER@/PulpControlShipping.cmake")
_pulp_cache_control_declarations(Leaked production-stripped "" FALSE)
_pulp_configure_control_shipping(Leaked "com.pulp.test.leaked" "Leaked")
add_executable(Leaked leak.cpp)
_pulp_attach_control_shipping(Leaked Leaked Standalone)
]=])
    file(READ "${_source}/CMakeLists.txt" _leak_cmake)
    string(REPLACE "@HELPER@" "${_installed_helper_dir}" _leak_cmake "${_leak_cmake}")
    file(WRITE "${_source}/CMakeLists.txt" "${_leak_cmake}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
        -DCMAKE_BUILD_TYPE=Release RESULT_VARIABLE _leak_configure
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _leak_configure EQUAL 0)
        message(FATAL_ERROR "could not configure ${case_name} negative fixture")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config Release --parallel 2
        RESULT_VARIABLE _leak_build OUTPUT_VARIABLE _leak_output ERROR_VARIABLE _leak_error)
    if(_leak_build EQUAL 0)
        message(FATAL_ERROR "${case_name} control leak was not blocked")
    endif()
    string(FIND "${_leak_output}${_leak_error}" "production-stripped artifact contains forbidden"
        _blocked_reason)
    if(_blocked_reason LESS 0)
        message(FATAL_ERROR
            "${case_name} failed for the wrong reason: ${_leak_output}${_leak_error}")
    endif()
endfunction()

expect_leak_blocked(endpoint-marker
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char leak[] = \"PULP_INSPECT_SHIPPING_MANIFEST_V1\";")
expect_leak_blocked(runtime-eval-marker
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char leak[] = \"PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1\";")
expect_leak_blocked(remote-view-marker
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char leak[] = \"PULP_REMOTE_VIEW_PARAMETER_AUTHORITY_V1\";")
expect_leak_blocked(remote-view-legacy-handler
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char leak[] = \"view.param_set\";")
expect_leak_blocked(capability-marker
    "#if defined(_MSC_VER)\n#define USED\n#else\n#define USED __attribute__((used))\n#endif\nextern \"C\" USED const volatile char leak[] = \"PULP_INSPECT_CAPABILITY_STATE_WRITE_V1\";")
expect_leak_blocked(symbol-leak
    "namespace pulp::inspect { class ControlBroker { public: int run(); }; int ControlBroker::run() { return 7; } }")

set(_dependency_source "${FIXTURE_DIR}/dependency-leak-source")
set(_dependency_build "${FIXTURE_DIR}/dependency-leak-build")
file(MAKE_DIRECTORY "${_dependency_source}")
file(WRITE "${_dependency_source}/dependency.cpp"
    "#if defined(_MSC_VER)\n#define USED __declspec(dllexport)\n#else\n#define USED __attribute__((used, visibility(\"default\")))\n#endif\nextern \"C\" USED const volatile char hidden_control[] = \"PULP_INSPECT_SHIPPING_MANIFEST_V1\";\nnamespace pulp::inspect { class ControlBroker { public: int run(); }; int ControlBroker::run() { return 7; } }\nextern \"C\" USED int control_dependency() { pulp::inspect::ControlBroker broker; return broker.run(); }\n")
file(WRITE "${_dependency_source}/main.cpp"
    "extern \"C\" int control_dependency(); int main() { return control_dependency(); }\n")
file(WRITE "${_dependency_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(ControlDependencyLeak LANGUAGES CXX)
include("@HELPER@/PulpControlShipping.cmake")
add_library(ForbiddenControl SHARED dependency.cpp)
set_target_properties(ForbiddenControl PROPERTIES OUTPUT_NAME "helpers")
_pulp_cache_control_declarations(Leaked production-stripped "" FALSE)
_pulp_configure_control_shipping(Leaked "com.pulp.test.leaked" "Leaked")
add_executable(Leaked main.cpp)
target_link_libraries(Leaked PRIVATE ForbiddenControl)
_pulp_attach_control_shipping(Leaked Leaked Standalone)
]=])
file(READ "${_dependency_source}/CMakeLists.txt" _dependency_cmake)
string(REPLACE "@HELPER@" "${_installed_helper_dir}"
    _dependency_cmake "${_dependency_cmake}")
file(WRITE "${_dependency_source}/CMakeLists.txt" "${_dependency_cmake}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dependency_source}"
    -B "${_dependency_build}" -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE _dependency_configure OUTPUT_QUIET ERROR_QUIET)
if(NOT _dependency_configure EQUAL 0)
    message(FATAL_ERROR "could not configure dependency leak fixture")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_dependency_build}"
    --config Release --parallel 2 RESULT_VARIABLE _dependency_leak_build
    OUTPUT_VARIABLE _dependency_output ERROR_VARIABLE _dependency_error)
if(_dependency_leak_build EQUAL 0)
    message(FATAL_ERROR "control dependency leak was not blocked")
endif()
string(FIND "${_dependency_output}${_dependency_error}"
    "production-stripped artifact contains forbidden control" _dependency_reason)
if(_dependency_reason LESS 0)
    message(FATAL_ERROR
        "dependency leak failed for the wrong reason: ${_dependency_output}${_dependency_error}")
endif()
