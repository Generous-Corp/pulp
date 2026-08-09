# Canonical capability-control declarations and artifact shipping evidence.
# Availability of optional control components never opts a product target in.

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
    "a3fc916f0a8da1724ae59bff2c94298c55d68c4c01b682224fd8605e05c0ca5b")

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

function(_pulp_control_json_escape output value)
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

function(_pulp_configure_control_shipping target bundle_id product_name)
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
    set(PULP_${target}_CONTROL_PROFILE "${_profile}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_CAPABILITIES "${_control_caps}"
        CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_BUNDLE_ID "${bundle_id}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_PRODUCT_NAME "${product_name}"
        CACHE INTERNAL "" FORCE)

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
    _pulp_control_json_escape(_json_target "${target}")
    _pulp_control_json_escape(_json_product_name "${product_name}")
    _pulp_control_json_escape(_json_bundle_id "${bundle_id}")
    _pulp_control_json_escape(_json_build_id
        "${PULP_${target}_CONTROL_BUILD_ID}")
    set(_manifest_content
        "{\n  \"schema\": \"dev.pulp.control/artifact-manifest@1\",\n  \"schema_version\": 1,\n  \"profile\": \"${_profile}\",\n  \"target\": \"${_json_target}\",\n  \"product_name\": \"${_json_product_name}\",\n  \"bundle_id\": \"${_json_bundle_id}\",\n  \"build_id\": \"${_json_build_id}\",\n  \"registry_digest\": \"${_control_registry_digest}\",\n  \"endpoint_included\": ${_shipping},\n  \"unsafe_runtime_eval_acknowledged\": ${_runtime_eval},\n  \"permission_terms\": [\"implemented\", \"built\", \"host_available\", \"activated\", \"policy_eligible\", \"client_granted\", \"session_live\"],\n  \"capabilities\": [${_json_control_caps}]\n}\n")
    file(GENERATE OUTPUT "${_manifest}" CONTENT "${_manifest_content}")
    string(SHA256 _manifest_digest "${_manifest_content}")
    set(PULP_${target}_INSPECTOR_MANIFEST "${_manifest}" CACHE INTERNAL "")

    set(PULP_${target}_CONTROL_MANIFEST_DIGEST "${_manifest_digest}" CACHE INTERNAL "" FORCE)
endfunction()

function(_pulp_attach_control_shipping target artifact_target artifact_format)
    if(NOT TARGET ${artifact_target})
        message(FATAL_ERROR "control shipping target does not exist: ${artifact_target}")
    endif()
    if(NOT artifact_format)
        message(FATAL_ERROR "control shipping format is required for ${artifact_target}")
    endif()

    set(_profile "${PULP_${target}_CONTROL_PROFILE}")
    set(_artifact_capabilities "${PULP_${target}_CONTROL_CAPABILITIES}")
    set(_control_manifest "${PULP_${target}_INSPECTOR_MANIFEST}")
    set(_control_manifest_digest "${PULP_${target}_CONTROL_MANIFEST_DIGEST}")
    # Only Standalone composes the control endpoint today. Mixed developer
    # builds keep every plug-in module on an independent stripped declaration.
    if(NOT artifact_format STREQUAL "Standalone" AND
       NOT _profile STREQUAL "production-stripped")
        set(_profile "production-stripped")
        set(_artifact_capabilities "")
        _pulp_control_json_escape(_artifact_json_target "${target}")
        _pulp_control_json_escape(_artifact_json_product
            "${PULP_${target}_CONTROL_PRODUCT_NAME}")
        _pulp_control_json_escape(_artifact_json_bundle
            "${PULP_${target}_CONTROL_BUNDLE_ID}")
        _pulp_control_json_escape(_artifact_json_build_id
            "${PULP_${target}_CONTROL_BUILD_ID}")
        set(_artifact_manifest_content
            "{\n  \"schema\": \"dev.pulp.control/artifact-manifest@1\",\n  \"schema_version\": 1,\n  \"profile\": \"production-stripped\",\n  \"target\": \"${_artifact_json_target}\",\n  \"product_name\": \"${_artifact_json_product}\",\n  \"bundle_id\": \"${_artifact_json_bundle}\",\n  \"build_id\": \"${_artifact_json_build_id}\",\n  \"registry_digest\": \"${_PULP_CONTROL_REGISTRY_DIGEST_V1}\",\n  \"endpoint_included\": false,\n  \"unsafe_runtime_eval_acknowledged\": false,\n  \"permission_terms\": [\"implemented\", \"built\", \"host_available\", \"activated\", \"policy_eligible\", \"client_granted\", \"session_live\"],\n  \"capabilities\": []\n}\n")
        get_filename_component(_artifact_manifest_dir "${_control_manifest}" DIRECTORY)
        set(_control_manifest
            "${_artifact_manifest_dir}/${target}.${artifact_format}.json")
        file(GENERATE OUTPUT "${_control_manifest}"
            CONTENT "${_artifact_manifest_content}")
        string(SHA256 _control_manifest_digest "${_artifact_manifest_content}")
    endif()
    string(MAKE_C_IDENTIFIER "${_profile}" _profile_identifier)
    string(TOUPPER "${_profile_identifier}" _profile_identifier)
    string(MAKE_C_IDENTIFIER "${artifact_format}" _format_identifier)
    string(TOUPPER "${_format_identifier}" _format_identifier)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _platform)
    string(MAKE_C_IDENTIFIER "${_platform}" _platform_identifier)
    string(TOUPPER "${_platform_identifier}" _platform_identifier)

    get_target_property(_architectures ${artifact_target} OSX_ARCHITECTURES)
    if(NOT _architectures OR _architectures STREQUAL "_architectures-NOTFOUND")
        set(_architectures "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    list(REMOVE_DUPLICATES _architectures)
    list(SORT _architectures)
    set(_json_architectures "")
    set(_architecture_markers "")
    foreach(_architecture IN LISTS _architectures)
        _pulp_control_json_escape(_json_architecture "${_architecture}")
        if(_json_architectures)
            string(APPEND _json_architectures ", ")
        endif()
        string(APPEND _json_architectures "\"${_json_architecture}\"")
        string(MAKE_C_IDENTIFIER "${_architecture}" _architecture_identifier)
        string(TOUPPER "${_architecture_identifier}" _architecture_identifier)
        string(APPEND _architecture_markers
            "\\0PULP_CONTROL_ARTIFACT_ARCH_${_architecture_identifier}_V1")
    endforeach()

    _pulp_control_json_escape(_json_artifact_target "${artifact_target}")
    _pulp_control_json_escape(_json_artifact_format "${artifact_format}")
    set(_ordinary_production false)
    if(_profile STREQUAL "production-stripped")
        set(_ordinary_production true)
    endif()
    list(LENGTH _artifact_capabilities _capability_count)
    set(_shipping_manifest_content
        "{\n  \"schema\": \"dev.pulp.control/shipping-artifact@1\",\n  \"schema_version\": 1,\n  \"target\": \"${target}\",\n  \"artifact_target\": \"${_json_artifact_target}\",\n  \"format\": \"${_json_artifact_format}\",\n  \"platform\": \"${_platform}\",\n  \"architectures\": [${_json_architectures}],\n  \"profile\": \"${_profile}\",\n  \"ordinary_production\": ${_ordinary_production},\n  \"capability_count\": ${_capability_count},\n  \"control_manifest_sha256\": \"${_control_manifest_digest}\"\n}\n")
    string(SHA256 _shipping_manifest_digest "${_shipping_manifest_content}")
    set(_shipping_dir "${CMAKE_BINARY_DIR}/pulp-control-shipping-manifests")
    file(MAKE_DIRECTORY "${_shipping_dir}")
    set(_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp-control-shipping-stamps")
    file(MAKE_DIRECTORY "${_stamp_dir}")
    set(_scan_stamp
        "${_stamp_dir}/${artifact_target}.${_format_identifier}.$<CONFIG>.stamp")
    set(_shipping_manifest
        "${_shipping_dir}/${target}.${artifact_format}.control-shipping.json")
    file(GENERATE OUTPUT "${_shipping_manifest}" CONTENT "${_shipping_manifest_content}")

    # This translation unit carries artifact identity only. Endpoint and
    # capability markers must come from the linked implementations themselves;
    # synthesizing them here would let an empty target satisfy its declaration.
    set(_standalone_marker "")
    if(artifact_format STREQUAL "Standalone")
        set(_standalone_marker "\\0PULP_STANDALONE_COMPONENT_V1")
    endif()
    set(_marker_source
        "${CMAKE_CURRENT_BINARY_DIR}/${target}_${artifact_target}_${_format_identifier}_control_shipping_marker.cpp")
    set(_marker_content
        "#if defined(_MSC_VER)\n#define PULP_SHIPPING_USED\n#else\n#define PULP_SHIPPING_USED __attribute__((used, visibility(\"default\")))\n#endif\nextern \"C\" PULP_SHIPPING_USED const volatile char pulp_control_shipping_${_format_identifier}_v1[] = \"PULP_CONTROL_PROFILE_${_profile_identifier}_V1\\0PULP_CONTROL_MANIFEST_SHA256_${_control_manifest_digest}_V1\\0PULP_CONTROL_SHIPPING_SHA256_${_shipping_manifest_digest}_V1\\0PULP_CONTROL_ARTIFACT_FORMAT_${_format_identifier}_V1\\0PULP_CONTROL_ARTIFACT_PLATFORM_${_platform_identifier}_V1${_architecture_markers}${_standalone_marker}\";\n")
    file(GENERATE OUTPUT "${_marker_source}" CONTENT "${_marker_content}")
    target_sources(${artifact_target} PRIVATE "${_marker_source}")

    add_custom_command(TARGET ${artifact_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_control_manifest}"
            "$<TARGET_FILE_DIR:${artifact_target}>/${target}.inspector-capabilities.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_shipping_manifest}"
            "$<TARGET_FILE_DIR:${artifact_target}>/${target}.${artifact_format}.control-shipping.json"
        COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=$<TARGET_FILE:${artifact_target}>
            -DMANIFEST=${_control_manifest}
            -DSHIPPING_MANIFEST=${_shipping_manifest}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DREPORT=$<TARGET_FILE_DIR:${artifact_target}>/${target}.${artifact_format}.control-shipping-report.json
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_control_shipping_artifact.cmake"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_scan_stamp}"
        VERBATIM)

    # POST_BUILD protects direct builds of the artifact. The persisted stamp
    # additionally makes scanner/manifests first-class dependencies, so an
    # install or normal ALL build re-scans an unchanged binary after policy
    # changes instead of trusting a stale report.
    set(_scanner "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_control_shipping_artifact.cmake")
    set(_scan_target "${artifact_target}_ControlShippingScan_${_format_identifier}")
    add_custom_command(
        OUTPUT "${_scan_stamp}"
        COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=$<TARGET_FILE:${artifact_target}>
            -DMANIFEST=${_control_manifest}
            -DSHIPPING_MANIFEST=${_shipping_manifest}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DREPORT=$<TARGET_FILE_DIR:${artifact_target}>/${target}.${artifact_format}.control-shipping-report.json
            -P "${_scanner}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_scan_stamp}"
        DEPENDS ${artifact_target} "${_control_manifest}"
            "${_shipping_manifest}" "${_scanner}"
        VERBATIM)
    add_custom_target(${_scan_target} ALL DEPENDS "${_scan_stamp}")
    set(PULP_${artifact_target}_CONTROL_SHIPPING_SCAN_TARGET
        "${_scan_target}" CACHE INTERNAL "" FORCE)
endfunction()

function(_pulp_control_shipping_scan_dependency output artifact_target)
    set(_scan_variable
        "PULP_${artifact_target}_CONTROL_SHIPPING_SCAN_TARGET")
    set(_scan_target "${${_scan_variable}}")
    if(NOT _scan_target OR NOT TARGET ${_scan_target})
        message(FATAL_ERROR
            "control shipping scan target is unavailable for ${artifact_target}")
    endif()
    set(${output} "${_scan_target}" PARENT_SCOPE)
endfunction()

function(_pulp_attach_inspector_shipping target artifact_target)
    _pulp_attach_control_shipping(${target} ${artifact_target} Standalone)
endfunction()

function(_pulp_inspector_json_escape output value)
    _pulp_control_json_escape(_escaped "${value}")
    set(${output} "${_escaped}" PARENT_SCOPE)
endfunction()

function(_pulp_configure_inspector_shipping target bundle_id product_name)
    _pulp_configure_control_shipping(${target} "${bundle_id}" "${product_name}")
endfunction()

cmake_policy(POP)
