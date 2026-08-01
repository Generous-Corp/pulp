# Declared shipping boundary for the standalone Development Inspector.
# Availability of optional SDK components never opts a product target in.

set(_PULP_INSPECTOR_SHIPPING_CAPABILITIES
    session.describe
    session.control
    state.read
    ui.read
    diagnostics.read
    logs.read
    capture.image
    trace.control
    trace.session.control
    state.write
    test.input
    authoring.tweaks
    telemetry.stream
    runtime.eval)

function(_pulp_inspector_json_escape output value)
    set(_escaped "${value}")
    string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    string(REPLACE "\n" "\\n" _escaped "${_escaped}")
    string(REPLACE "\r" "\\r" _escaped "${_escaped}")
    string(REPLACE "\t" "\\t" _escaped "${_escaped}")
    set(_hex_digits 0 1 2 3 4 5 6 7 8 9 A B C D E F)
    foreach(_code RANGE 1 31)
        string(ASCII ${_code} _control_character)
        math(EXPR _high_nibble "${_code} / 16")
        math(EXPR _low_nibble "${_code} % 16")
        list(GET _hex_digits ${_high_nibble} _high_hex)
        list(GET _hex_digits ${_low_nibble} _low_hex)
        string(REPLACE "${_control_character}"
            "\\u00${_high_hex}${_low_hex}" _escaped "${_escaped}")
    endforeach()
    set(${output} "${_escaped}" PARENT_SCOPE)
endfunction()

function(_pulp_configure_inspector_shipping target bundle_id product_name)
    set(_caps "${PULP_${target}_INSPECTOR_CAPABILITIES}")
    foreach(_cap IN LISTS _caps)
        if(NOT _cap IN_LIST _PULP_INSPECTOR_SHIPPING_CAPABILITIES)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): unknown inspector shipping capability '${_cap}'")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _caps)
    list(SORT _caps)

    set(_shipping false)
    set(_runtime_eval false)
    if(PULP_${target}_SHIP_INSPECTOR)
        set(_shipping true)
    endif()
    if(PULP_${target}_SHIP_INSPECTOR_RUNTIME_EVAL)
        set(_runtime_eval true)
    endif()

    set(_json_caps "")
    set(_marker_caps "")
    set(_registration_caps "")
    foreach(_cap IN LISTS _caps)
        if(_json_caps)
            string(APPEND _json_caps ", ")
        endif()
        string(APPEND _json_caps "\"${_cap}\"")
        string(MAKE_C_IDENTIFIER "${_cap}" _cap_identifier)
        string(TOUPPER "${_cap_identifier}" _cap_identifier)
        string(APPEND _marker_caps
            "PULP_INSPECT_CAPABILITY_${_cap_identifier}_V1\\0")
        if(_registration_caps)
            string(APPEND _registration_caps ", ")
        endif()
        string(APPEND _registration_caps "\"${_cap}\"")
    endforeach()

    set(_manifest_dir "${CMAKE_BINARY_DIR}/pulp-inspector-manifests")
    if(PULP_${target}_INSPECTOR_MANIFEST_DIRECTORY)
        set(_manifest_dir "${PULP_${target}_INSPECTOR_MANIFEST_DIRECTORY}")
    endif()
    file(MAKE_DIRECTORY "${_manifest_dir}")
    set(_manifest "${_manifest_dir}/${target}.json")
    _pulp_inspector_json_escape(_json_target "${target}")
    _pulp_inspector_json_escape(_json_product_name "${product_name}")
    _pulp_inspector_json_escape(_json_bundle_id "${bundle_id}")
    file(GENERATE OUTPUT "${_manifest}" CONTENT
        "{\n  \"schema_version\": 1,\n  \"target\": \"${_json_target}\",\n  \"product_name\": \"${_json_product_name}\",\n  \"bundle_id\": \"${_json_bundle_id}\",\n  \"shipping_override\": ${_shipping},\n  \"unsafe_runtime_eval_acknowledged\": ${_runtime_eval},\n  \"activation\": \"product-owned; runtime default off\",\n  \"capabilities\": [${_json_caps}]\n}\n")
    set(PULP_${target}_INSPECTOR_MANIFEST "${_manifest}" CACHE INTERNAL "")

    if(PULP_${target}_SHIP_INSPECTOR)
        set(_marker_source "${CMAKE_CURRENT_BINARY_DIR}/${target}_inspector_shipping_marker.cpp")
        file(GENERATE OUTPUT "${_marker_source}" CONTENT
            "#include <pulp/format/detail/standalone_inspector.hpp>\n#include <string>\n#include <vector>\n#if defined(_MSC_VER)\n#define PULP_SHIPPING_USED __declspec(selectany)\n#else\n#define PULP_SHIPPING_USED __attribute__((used, visibility(\"default\")))\n#endif\nextern \"C\" PULP_SHIPPING_USED const char pulp_inspector_shipping_manifest_v1[] = \"PULP_INSPECT_SHIPPING_MANIFEST_V1\\0${_marker_caps}\";\nnamespace { struct PulpInspectorShippingRegistration { PulpInspectorShippingRegistration() { pulp::format::detail::set_standalone_inspector_shipping_capabilities(std::vector<std::string>{${_registration_caps}}); } } pulp_inspector_shipping_registration; }\n")
        set(PULP_${target}_INSPECTOR_MARKER_SOURCE "${_marker_source}" CACHE INTERNAL "")
    endif()
endfunction()

function(_pulp_attach_inspector_shipping target artifact_target)
    if(PULP_${target}_SHIP_INSPECTOR)
        target_sources(${artifact_target} PRIVATE
            "${PULP_${target}_INSPECTOR_MARKER_SOURCE}")
    endif()
    add_custom_command(TARGET ${artifact_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PULP_${target}_INSPECTOR_MANIFEST}"
            "$<TARGET_FILE_DIR:${artifact_target}>/${target}.inspector-capabilities.json"
        COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=$<TARGET_FILE:${artifact_target}>
            -DMANIFEST=${PULP_${target}_INSPECTOR_MANIFEST}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_inspector_shipping_artifact.cmake"
        VERBATIM)
endfunction()
