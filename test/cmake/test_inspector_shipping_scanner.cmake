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

# The public plugin helper must not let a declaration outrun the shipped
# host-side implementation. A raw shipping-helper fixture below still proves
# the manifest/scanner contract independently, while pulp_add_plugin fails
# before it can emit endpoint_included=true for ordinary pulp::standalone.
set(_public_control_source "${FIXTURE_DIR}/public-control-source")
set(_public_control_build "${FIXTURE_DIR}/public-control-build")
file(REMOVE_RECURSE "${_public_control_source}" "${_public_control_build}")
file(MAKE_DIRECTORY "${_public_control_source}")
file(WRITE "${_public_control_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(PublicControlDeclaration NONE)\n"
    "add_library(pulp-format INTERFACE)\n"
    "add_library(pulp::format ALIAS pulp-format)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake\")\n"
    "pulp_add_plugin(ControlTarget FORMATS Standalone BUNDLE_ID dev.pulp.control CONTROL_PROFILE developer-local CONTROL_CAPABILITIES dev.pulp.instance/read@1)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_public_control_source}"
    -B "${_public_control_build}"
    RESULT_VARIABLE _public_control_result
    OUTPUT_VARIABLE _public_control_output ERROR_VARIABLE _public_control_error)
if(_public_control_result EQUAL 0)
    message(FATAL_ERROR
        "ordinary pulp_add_plugin accepted an unavailable control endpoint")
endif()
set(_public_control_combined
    "${_public_control_output}${_public_control_error}")
string(REGEX REPLACE "[ \t\r\n]+" " " _public_control_combined
    "${_public_control_combined}")
string(FIND "${_public_control_combined}"
    "requires the dedicated canonical Standalone host adapter"
    _public_control_diagnostic)
if(_public_control_diagnostic LESS 0)
    message(FATAL_ERROR
        "ordinary control declaration did not fail with the canonical adapter diagnostic: "
        "${_public_control_output}${_public_control_error}")
endif()

# Reconfigure the same build directory after withdrawing critical authority.
# Cache-backed declarations must reflect the current CMakeLists on every run.
set(_reconfigure_source "${FIXTURE_DIR}/control-reconfigure-source")
set(_reconfigure_build "${FIXTURE_DIR}/control-reconfigure-build")
file(REMOVE_RECURSE "${_reconfigure_source}" "${_reconfigure_build}")
file(MAKE_DIRECTORY "${_reconfigure_source}")
file(WRITE "${_reconfigure_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(ControlReconfigure NONE)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake\")\n"
    "option(ENABLE_CRITICAL_CONTROL \"\" OFF)\n"
    "if(ENABLE_CRITICAL_CONTROL)\n"
    "  _pulp_cache_control_declarations(Fixture research-unsafe \"dev.pulp.session/control@1;dev.pulp.runtime/evaluate@1\" TRUE)\n"
    "else()\n"
    "  _pulp_cache_control_declarations(Fixture production-stripped \"\" FALSE)\n"
    "endif()\n"
    "file(WRITE \"\${CMAKE_BINARY_DIR}/observed.txt\" \"\${PULP_Fixture_CONTROL_PROFILE}|\${PULP_Fixture_CONTROL_CAPABILITIES}|\${PULP_Fixture_CONTROL_UNSAFE_RUNTIME_EVAL_ACKNOWLEDGED}\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_reconfigure_source}"
    -B "${_reconfigure_build}" -DENABLE_CRITICAL_CONTROL=ON
    RESULT_VARIABLE _critical_configure_result OUTPUT_QUIET ERROR_QUIET)
if(NOT _critical_configure_result EQUAL 0)
    message(FATAL_ERROR "could not configure critical control cache fixture")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_reconfigure_source}"
    -B "${_reconfigure_build}" -DENABLE_CRITICAL_CONTROL=OFF
    RESULT_VARIABLE _stripped_reconfigure_result OUTPUT_QUIET ERROR_QUIET)
if(NOT _stripped_reconfigure_result EQUAL 0)
    message(FATAL_ERROR "could not reconfigure stripped control cache fixture")
endif()
file(READ "${_reconfigure_build}/observed.txt" _reconfigured_declarations)
if(NOT _reconfigured_declarations STREQUAL "production-stripped||FALSE")
    message(FATAL_ERROR
        "withdrawn control authority remained cached: ${_reconfigured_declarations}")
endif()

set(_unsupported_trace_script "${FIXTURE_DIR}/unsupported-trace.cmake")
file(WRITE "${_unsupported_trace_script}"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake\")\n"
    "set(PULP_TraceFixture_CONTROL_PROFILE developer-local)\n"
    "set(PULP_TraceFixture_CONTROL_CAPABILITIES dev.pulp.trace/control@1)\n"
    "_pulp_configure_inspector_shipping(TraceFixture com.pulp.trace TraceFixture)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_unsupported_trace_script}"
    RESULT_VARIABLE _unsupported_trace_result
    OUTPUT_QUIET ERROR_QUIET)
if(_unsupported_trace_result EQUAL 0)
    message(FATAL_ERROR "unsupported control trace capability was accepted")
endif()

function(_authoring_case name body expect_success)
    set(_script "${FIXTURE_DIR}/authoring-${name}.cmake")
    file(WRITE "${_script}"
        "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake\")\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(expect_success AND NOT _result EQUAL 0)
        message(FATAL_ERROR
            "${name}: valid control authoring was rejected: ${_output}${_error}")
    elseif(NOT expect_success AND _result EQUAL 0)
        message(FATAL_ERROR "${name}: invalid control authoring was accepted")
    endif()
endfunction()

_authoring_case(developer-contract-id
    "set(PULP_ControlFixture_CONTROL_PROFILE developer-local)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1;dev.pulp.state/read@1)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    TRUE)
_authoring_case(production-with-capability
    "set(PULP_ControlFixture_CONTROL_PROFILE production-stripped)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    FALSE)
_authoring_case(support-with-mutation
    "set(PULP_ControlFixture_CONTROL_PROFILE support-diagnostics)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.session/control@1;dev.pulp.state/parameter-gesture@1)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    FALSE)
_authoring_case(eval-without-acknowledgement
    "set(PULP_ControlFixture_CONTROL_PROFILE research-unsafe)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.session/control@1;dev.pulp.runtime/evaluate@1)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    FALSE)
_authoring_case(acknowledgement-without-eval
    "set(PULP_ControlFixture_CONTROL_PROFILE research-unsafe)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1)\nset(PULP_ControlFixture_CONTROL_UNSAFE_RUNTIME_EVAL_ACKNOWLEDGED TRUE)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    FALSE)
_authoring_case(control-without-bundle-id
    "set(PULP_ControlFixture_CONTROL_PROFILE developer-local)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1)\n_pulp_configure_inspector_shipping(ControlFixture \"\" ControlFixture)"
    FALSE)
_authoring_case(stripped-without-bundle-id
    "set(PULP_ControlFixture_CONTROL_PROFILE production-stripped)\n_pulp_configure_inspector_shipping(ControlFixture \"\" ControlFixture)"
    TRUE)
_authoring_case(unknown-control-profile
    "set(PULP_ControlFixture_CONTROL_PROFILE surprise-profile)\nset(PULP_ControlFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1)\n_pulp_configure_inspector_shipping(ControlFixture com.pulp.control ControlFixture)"
    FALSE)

# Simulate the installed SDK layout: the helper must be self-contained and
# must not read ../../inspect from its installed lib/cmake/Pulp directory.
set(_installed_helper_dir "${FIXTURE_DIR}/installed/lib/cmake/Pulp")
file(MAKE_DIRECTORY "${_installed_helper_dir}")
file(COPY "${PULP_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake"
    "${PULP_SOURCE_DIR}/tools/cmake/PulpControlShipping.cmake"
    DESTINATION "${_installed_helper_dir}")
set(_installed_helper_script "${FIXTURE_DIR}/installed-helper.cmake")
file(WRITE "${_installed_helper_script}"
    "include(\"${_installed_helper_dir}/PulpInspectorShipping.cmake\")\n"
    "set(PULP_InstalledFixture_CONTROL_PROFILE developer-local)\n"
    "set(PULP_InstalledFixture_CONTROL_CAPABILITIES dev.pulp.instance/read@1)\n"
    "_pulp_configure_inspector_shipping(InstalledFixture com.pulp.installed InstalledFixture)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_installed_helper_script}"
    RESULT_VARIABLE _installed_helper_result
    OUTPUT_VARIABLE _installed_helper_output ERROR_VARIABLE _installed_helper_error)
if(NOT _installed_helper_result EQUAL 0)
    message(FATAL_ERROR
        "installed inspector shipping helper is not self-contained: "
        "${_installed_helper_output}${_installed_helper_error}")
endif()
