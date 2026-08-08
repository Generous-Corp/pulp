# Declared shipping boundary for the standalone Development Inspector.
# Availability of optional SDK components never opts a product target in.

# Functions retain the policy state in effect when they are defined. Set the
# policy used by IN_LIST explicitly so this installed helper also behaves when
# loaded from cmake -P, where callers may not establish project policies.
cmake_policy(PUSH)
cmake_policy(SET CMP0057 NEW)

set(_PULP_INSPECTOR_SHIPPING_CAPABILITIES
    session.describe
    session.control
    state.read
    ui.read
    diagnostics.read
    logs.read
    capture.image
    state.write
    test.input
    authoring.tweaks
    telemetry.stream
    runtime.eval)

set(_PULP_CONTROL_CAPABILITIES
    dev.pulp.instance/read@1
    dev.pulp.session/control@1
    dev.pulp.state/read@1
    dev.pulp.ui/observe@1
    dev.pulp.diagnostics/read@1
    dev.pulp.logs/read@1
    dev.pulp.ui/capture@1
    dev.pulp.state/parameter-gesture@1
    dev.pulp.test/input@1
    dev.pulp.authoring/tweaks@1
    dev.pulp.telemetry/subscribe@1
    dev.pulp.runtime/evaluate@1)

# Installed copies of this helper cannot reach back into the source tree. The
# truth checker pins this value to control_registry_digest.inc.
set(_PULP_CONTROL_REGISTRY_DIGEST_V1
    "d991067d0572d4f6fb5c2facac0e6f0708a5b0fad0262b552f7cde5b352197b1")

function(_pulp_cache_control_declarations target profile capabilities eval_ack)
    # A target's declarations are configure-time truth, not sticky user
    # preferences. FORCE is required so removing a capability (especially
    # runtime evaluation) cannot leave stale authority in an existing build.
    set(PULP_${target}_CONTROL_PROFILE "${profile}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_CAPABILITIES "${capabilities}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_UNSAFE_RUNTIME_EVAL_ACKNOWLEDGED
        "${eval_ack}" CACHE INTERNAL "" FORCE)
endfunction()

function(_pulp_control_capability_projection output_contract input)
    list(FIND _PULP_CONTROL_CAPABILITIES "${input}" _contract_index)
    if(_contract_index GREATER_EQUAL 0)
        set(_contract "${input}")
    else()
        set(_contract "")
    endif()
    set(${output_contract} "${_contract}" PARENT_SCOPE)
endfunction()

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
    set(_control_registry_digest "${_PULP_CONTROL_REGISTRY_DIGEST_V1}")
    string(LENGTH "${_control_registry_digest}" _control_registry_digest_length)
    if(NOT _control_registry_digest_length EQUAL 64)
        message(FATAL_ERROR "invalid frozen control registry digest")
    endif()
    set(_profile "${PULP_${target}_CONTROL_PROFILE}")
    if(NOT _profile)
        set(_profile "production-stripped")
    endif()
    set(_control_profiles production-stripped developer-local
        test-deterministic support-diagnostics research-unsafe)
    if(NOT _profile IN_LIST _control_profiles)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): unknown CONTROL_PROFILE '${_profile}'")
    endif()

    set(_declared_caps "${PULP_${target}_CONTROL_CAPABILITIES}")
    set(_control_caps "")
    foreach(_declared_cap IN LISTS _declared_caps)
        _pulp_control_capability_projection(_control_cap "${_declared_cap}")
        if(NOT _control_cap)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): unknown control capability '${_declared_cap}'")
        endif()
        list(APPEND _control_caps "${_control_cap}")
    endforeach()
    list(REMOVE_DUPLICATES _control_caps)
    list(SORT _control_caps)

    set(_controller_caps
        state.write test.input authoring.tweaks runtime.eval)
    foreach(_control_cap IN LISTS _control_caps)
        list(FIND _PULP_CONTROL_CAPABILITIES "${_control_cap}" _cap_index)
        list(GET _PULP_INSPECTOR_SHIPPING_CAPABILITIES ${_cap_index} _cap)
        if(_cap IN_LIST _controller_caps AND
           NOT "dev.pulp.session/control@1" IN_LIST _control_caps)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): '${_cap}' requires dev.pulp.session/control@1")
        endif()
    endforeach()

    set(_shipping false)
    set(_runtime_eval false)
    if(_control_caps)
        set(_shipping true)
    endif()
    if(PULP_${target}_CONTROL_UNSAFE_RUNTIME_EVAL_ACKNOWLEDGED)
        set(_runtime_eval true)
    endif()

    if(_profile STREQUAL "production-stripped" AND _control_caps)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): production-stripped forbids CONTROL_CAPABILITIES")
    endif()
    if(NOT _profile STREQUAL "production-stripped" AND NOT _control_caps)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): ${_profile} requires an explicit CONTROL_CAPABILITIES list")
    endif()
    if(NOT _profile STREQUAL "production-stripped" AND NOT bundle_id)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): BUNDLE_ID is required when a control endpoint is declared")
    endif()
    if("dev.pulp.runtime/evaluate@1" IN_LIST _control_caps AND NOT _profile STREQUAL "research-unsafe")
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): dev.pulp.runtime/evaluate@1 is restricted to research-unsafe")
    endif()
    if("dev.pulp.runtime/evaluate@1" IN_LIST _control_caps AND NOT _runtime_eval)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): dev.pulp.runtime/evaluate@1 requires ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL")
    endif()
    if(_runtime_eval AND NOT "dev.pulp.runtime/evaluate@1" IN_LIST _control_caps)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL does not grant dev.pulp.runtime/evaluate@1")
    endif()
    if(_profile STREQUAL "support-diagnostics")
        set(_support_caps session.describe state.read diagnostics.read logs.read)
        foreach(_control_cap IN LISTS _control_caps)
            list(FIND _PULP_CONTROL_CAPABILITIES "${_control_cap}" _cap_index)
            list(GET _PULP_INSPECTOR_SHIPPING_CAPABILITIES ${_cap_index} _cap)
            if(NOT _cap IN_LIST _support_caps)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): support-diagnostics forbids '${_cap}'")
            endif()
        endforeach()
    endif()
    set(_json_control_caps "")
    foreach(_control_cap IN LISTS _control_caps)
        if(_json_control_caps)
            string(APPEND _json_control_caps ", ")
        endif()
        string(APPEND _json_control_caps "\"${_control_cap}\"")
    endforeach()

    set(_manifest_dir "${CMAKE_BINARY_DIR}/pulp-inspector-manifests")
    if(PULP_${target}_INSPECTOR_MANIFEST_DIRECTORY)
        set(_manifest_dir "${PULP_${target}_INSPECTOR_MANIFEST_DIRECTORY}")
    endif()
    file(MAKE_DIRECTORY "${_manifest_dir}")
    set(_manifest "${_manifest_dir}/${target}.json")
    string(LENGTH "${PULP_${target}_CONTROL_BUILD_ID}"
        _control_build_id_length)
    if(NOT PULP_${target}_CONTROL_BUILD_ID MATCHES "^build:[0-9a-f]+$" OR
       NOT _control_build_id_length EQUAL 38)
        string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef _control_build_nonce)
        set(PULP_${target}_CONTROL_BUILD_ID "build:${_control_build_nonce}"
            CACHE INTERNAL "Opaque control build-tree identity" FORCE)
    endif()
    _pulp_inspector_json_escape(_json_target "${target}")
    _pulp_inspector_json_escape(_json_product_name "${product_name}")
    _pulp_inspector_json_escape(_json_bundle_id "${bundle_id}")
    _pulp_inspector_json_escape(_json_build_id
        "${PULP_${target}_CONTROL_BUILD_ID}")
    set(_manifest_content
        "{\n  \"schema\": \"dev.pulp.control/artifact-manifest@1\",\n  \"schema_version\": 1,\n  \"profile\": \"${_profile}\",\n  \"target\": \"${_json_target}\",\n  \"product_name\": \"${_json_product_name}\",\n  \"bundle_id\": \"${_json_bundle_id}\",\n  \"build_id\": \"${_json_build_id}\",\n  \"registry_digest\": \"${_control_registry_digest}\",\n  \"endpoint_included\": ${_shipping},\n  \"unsafe_runtime_eval_acknowledged\": ${_runtime_eval},\n  \"permission_terms\": [\"implemented\", \"built\", \"host_available\", \"activated\", \"policy_eligible\", \"client_granted\", \"session_live\"],\n  \"capabilities\": [${_json_control_caps}]\n}\n")
    file(GENERATE OUTPUT "${_manifest}" CONTENT "${_manifest_content}")
    string(SHA256 _manifest_digest "${_manifest_content}")
    set(PULP_${target}_INSPECTOR_MANIFEST "${_manifest}" CACHE INTERNAL "")

    set(_marker_source "${CMAKE_CURRENT_BINARY_DIR}/${target}_inspector_shipping_marker.cpp")
    string(MAKE_C_IDENTIFIER "${_profile}" _profile_identifier)
    string(TOUPPER "${_profile_identifier}" _profile_identifier)
    set(_marker_content
        "#if defined(_MSC_VER)\n#define PULP_SHIPPING_USED\n#else\n#define PULP_SHIPPING_USED __attribute__((used, visibility(\"default\")))\n#endif\nextern \"C\" PULP_SHIPPING_USED const volatile char pulp_standalone_component_v1[] = \"PULP_STANDALONE_COMPONENT_V1\";\nextern \"C\" PULP_SHIPPING_USED const volatile char pulp_control_profile_v1[] = \"PULP_CONTROL_PROFILE_${_profile_identifier}_V1\\0PULP_CONTROL_MANIFEST_SHA256_${_manifest_digest}_V1\";\n")
    file(GENERATE OUTPUT "${_marker_source}" CONTENT "${_marker_content}")
    set(PULP_${target}_INSPECTOR_MARKER_SOURCE "${_marker_source}" CACHE INTERNAL "")
endfunction()

function(_pulp_attach_inspector_shipping target artifact_target)
    target_sources(${artifact_target} PRIVATE
        "${PULP_${target}_INSPECTOR_MARKER_SOURCE}")
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

cmake_policy(POP)
