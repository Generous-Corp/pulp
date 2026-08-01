if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
set(_scanner "${PULP_SOURCE_DIR}/tools/cmake/check_inspector_shipping_artifact.cmake")
include("${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake")
string(ASCII 8 _backspace)
_pulp_inspector_json_escape(
    _escaped_metadata "quoted \"name\"\\path\nline${_backspace}")
if(NOT _escaped_metadata STREQUAL
   "quoted \\\"name\\\"\\\\path\\nline\\u0008")
    message(FATAL_ERROR "inspector manifest metadata JSON escaping regressed")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

set(_unsupported_trace_script "${FIXTURE_DIR}/unsupported-trace.cmake")
file(WRITE "${_unsupported_trace_script}"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake\")\n"
    "set(PULP_TraceFixture_INSPECTOR_CAPABILITIES trace.control)\n"
    "set(PULP_TraceFixture_SHIP_INSPECTOR TRUE)\n"
    "set(PULP_TraceFixture_SHIP_INSPECTOR_RUNTIME_EVAL FALSE)\n"
    "_pulp_configure_inspector_shipping(TraceFixture com.pulp.trace TraceFixture)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_unsupported_trace_script}"
    RESULT_VARIABLE _unsupported_trace_result
    OUTPUT_QUIET ERROR_QUIET)
if(_unsupported_trace_result EQUAL 0)
    message(FATAL_ERROR "unsupported standalone trace capability was accepted")
endif()

function(_scan name binary manifest expect_success)
    set(_binary_path "${FIXTURE_DIR}/${name}.bin")
    set(_manifest_path "${FIXTURE_DIR}/${name}.json")
    file(WRITE "${_binary_path}" "${binary}")
    file(WRITE "${_manifest_path}" "${manifest}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -DARTIFACT=${_binary_path} -DMANIFEST=${_manifest_path} -P "${_scanner}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(expect_success AND NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: expected scan success: ${_output}${_error}")
    elseif(NOT expect_success AND _result EQUAL 0)
        message(FATAL_ERROR "${name}: mutation unexpectedly passed")
    endif()
endfunction()

set(_ordinary_manifest
    "{\"shipping_override\": false,\"unsafe_runtime_eval_acknowledged\": false}")
set(_shipping_manifest
    "{\"shipping_override\": true,\"unsafe_runtime_eval_acknowledged\": false}")
set(_eval_manifest
    "{\"shipping_override\": true,\"unsafe_runtime_eval_acknowledged\": true}")

_scan(ordinary "ordinary-product" "${_ordinary_manifest}" TRUE)
_scan(developer "PULP_INSPECT_SHIPPING_MANIFEST_V1" "${_shipping_manifest}" TRUE)
_scan(developer-eval
    "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1"
    "${_eval_manifest}" TRUE)
_scan(missing-marker "ordinary-product" "${_shipping_manifest}" FALSE)
_scan(unrelated-listener-name "InspectorServer DiscoveryPublisher publish_discovery_record"
    "${_ordinary_manifest}" TRUE)
_scan(eval-with-generic-override
    "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1"
    "${_shipping_manifest}" FALSE)
_scan(eval-ack-without-component
    "PULP_INSPECT_SHIPPING_MANIFEST_V1" "${_eval_manifest}" FALSE)
_scan(undeclared-capability-marker
    "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_CAPABILITY_UI_READ_V1"
    "${_shipping_manifest}" FALSE)
_scan(missing-capability-marker
    "PULP_INSPECT_SHIPPING_MANIFEST_V1"
    "{\"shipping_override\": true,\"unsafe_runtime_eval_acknowledged\": false,\"capabilities\":[\"ui.read\"]}"
    FALSE)
