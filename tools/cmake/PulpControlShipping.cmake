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
    gpu.health.read
    render.offline
    ui.read
    diagnostics.read
    logs.read
    capture.image
    ui.input
    trace.control
    trace.session.control
    state.write
    test.input
    authoring.tweaks
    telemetry.stream
    runtime.reload
    runtime.eval
    artifact.read
    sequencer.state.read
    sequencer.state.edit
    sequencer.transport.loop.read
    sequencer.transport.loop.write
    timeline.document.session
    graph.sample-region.read
    graph.sample-region.edit
    graph.file.reload
    graph.modulation-route.edit
    vendor.read
    vendor.gesture
    vendor.render
    vendor.destructive
    unavailable)

set(_PULP_CONTROL_CAPABILITIES
    dev.pulp.instance/read@1
    dev.pulp.session/control@1
    dev.pulp.state/read@1
    dev.pulp.gpu/health.read@1
    dev.pulp.render/offline@1
    dev.pulp.ui/observe@1
    dev.pulp.diagnostics/read@1
    dev.pulp.logs/read@1
    dev.pulp.ui/capture@1
    dev.pulp.ui/input@1
    dev.pulp.trace/control@1
    dev.pulp.trace/session-control@1
    dev.pulp.state/parameter-gesture@1
    dev.pulp.test/input@1
    dev.pulp.authoring/tweaks@1
    dev.pulp.telemetry/subscribe@1
    dev.pulp.runtime/reload@1
    dev.pulp.runtime/evaluate@1
    dev.pulp.artifact/read@1
    dev.pulp.sequencer/state.read@1
    dev.pulp.sequencer/state.edit@1
    dev.pulp.sequencer/transport.loop.read@1
    dev.pulp.sequencer/transport.loop.write@1
    dev.pulp.timeline/document-session@1
    dev.pulp.graph/sample-region.read@1
    dev.pulp.graph/sample-region.edit@1
    dev.pulp.graph/file.reload@1
    dev.pulp.graph/modulation-route.edit@1
    dev.pulp.vendor/read@1
    dev.pulp.vendor/gesture@1
    dev.pulp.vendor/render@1
    dev.pulp.vendor/destructive@1
    dev.pulp.unavailable/operation@1)

# Registry presence pre-stages schema/client compatibility but is not permission
# for a product to advertise an operation whose host adapter does not exist yet.
# Keep this list as the fail-closed gate for any future registry-first operation;
# GPU health is wired by the exact Standalone provider and view adapter.
set(_PULP_CONTROL_UNWIRED_CAPABILITIES)

# Installed copies of this helper cannot reach back into the source tree. The
# truth checker pins this value to control_registry_digest.inc.
set(_PULP_CONTROL_REGISTRY_DIGEST_V1
    "02929f4004e5a7234fad4132a503fdaaf2dcd6196a20f3dca0a3351b826d6954")

function(_pulp_cache_control_declarations target profile capabilities eval_ack)
    # A target's declarations are configure-time truth, not sticky user
    # preferences. FORCE is required so removing a capability (especially
    # runtime evaluation) cannot leave stale authority in an existing build.
    set(PULP_${target}_CONTROL_PROFILE "${profile}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_CAPABILITIES "${capabilities}" CACHE INTERNAL "" FORCE)
    set(PULP_${target}_CONTROL_UNSAFE_RUNTIME_EVAL_ACKNOWLEDGED
        "${eval_ack}" CACHE INTERNAL "" FORCE)
endfunction()

# Records the target's vendor operation declaration (an absolute path, or empty)
# and the sources that bind its handlers. Those sources build only into the
# controlled Standalone and its broker-launched host, never into other formats.
# FORCE keeps removal effective in an existing build tree, like the other
# control declarations.
function(_pulp_cache_control_vendor_operations target declaration)
    set(_sources "")
    foreach(_source IN LISTS ARGN)
        if(NOT IS_ABSOLUTE "${_source}")
            set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
        endif()
        cmake_path(NORMAL_PATH _source)
        list(APPEND _sources "${_source}")
    endforeach()
    set(PULP_${target}_CONTROL_VENDOR_SOURCES "${_sources}" CACHE INTERNAL "" FORCE)
    set(_path "")
    if(declaration)
        if(IS_ABSOLUTE "${declaration}")
            set(_path "${declaration}")
        else()
            set(_path "${CMAKE_CURRENT_SOURCE_DIR}/${declaration}")
        endif()
        cmake_path(NORMAL_PATH _path)
    endif()
    set(PULP_${target}_CONTROL_VENDOR_OPERATIONS "${_path}" CACHE INTERNAL "" FORCE)
endfunction()

set(_PULP_CONTROL_SHIPPING_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Writes the agent tool kit's vendor operation declaration for one vendor id
# and returns its path, ready for CONTROL_VENDOR_OPERATIONS. EXCLUDE drops whole
# operation groups the target does not bind (presets, patch, notes, render):
#   pulp_control_agent_toolkit_declaration(_ops VENDOR com.acme EXCLUDE presets)
# CONTROL_CAPABILITIES must then list exactly the tiers that remain.
function(pulp_control_agent_toolkit_declaration output)
    cmake_parse_arguments(PARSE_ARGV 1 _kit "" "VENDOR" "EXCLUDE")
    if(NOT _kit_VENDOR MATCHES "^[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)+$")
        message(FATAL_ERROR
            "pulp_control_agent_toolkit_declaration: VENDOR must be a lowercase reverse-DNS id")
    endif()
    foreach(_group IN LISTS _kit_EXCLUDE)
        if(NOT _group MATCHES "^(presets|patch|notes|render)$")
            message(FATAL_ERROR
                "pulp_control_agent_toolkit_declaration: unknown EXCLUDE group '${_group}'")
        endif()
    endforeach()
    set(PULP_AGENT_TOOLKIT_VENDOR "${_kit_VENDOR}")
    file(READ "${_PULP_CONTROL_SHIPPING_DIR}/control_agent_toolkit_operations.json.in" _template)
    string(CONFIGURE "${_template}" _document @ONLY)
    string(JSON _count LENGTH "${_document}" operations)
    math(EXPR _index "${_count} - 1")
    while(_index GREATER_EQUAL 0)
        string(JSON _id GET "${_document}" operations ${_index} id)
        foreach(_group IN LISTS _kit_EXCLUDE)
            if(_id MATCHES "/${_group}\\.")
                string(JSON _document REMOVE "${_document}" operations ${_index})
                break()
            endif()
        endforeach()
        math(EXPR _index "${_index} - 1")
    endwhile()
    string(JSON _count LENGTH "${_document}" operations)
    if(_count EQUAL 0)
        message(FATAL_ERROR "pulp_control_agent_toolkit_declaration: EXCLUDE removed every operation")
    endif()
    string(SHA256 _suffix "${_kit_EXCLUDE}")
    string(SUBSTRING "${_suffix}" 0 12 _suffix)
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/pulp-agent-toolkit-${_kit_VENDOR}-${_suffix}.json")
    file(CONFIGURE OUTPUT "${_path}" CONTENT "${_document}\n" @ONLY)
    set(${output} "${_path}" PARENT_SCOPE)
endfunction()

# Reads the declaration and returns its manifest member, or empty. The exact
# bytes are embedded as one JSON string so CMake and the broker produce the
# same canonical manifest; the broker performs the complete validation.
function(_pulp_control_vendor_operations_member output target control_caps)
    set(_path "${PULP_${target}_CONTROL_VENDOR_OPERATIONS}")
    set(_vendor_caps
        dev.pulp.vendor/read@1 dev.pulp.vendor/gesture@1
        dev.pulp.vendor/render@1 dev.pulp.vendor/destructive@1)
    if(NOT _path)
        foreach(_cap IN LISTS _vendor_caps)
            if(_cap IN_LIST control_caps)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): '${_cap}' requires a CONTROL_VENDOR_OPERATIONS declaration")
            endif()
        endforeach()
        set(${output} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${_path}" OR IS_DIRECTORY "${_path}")
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_VENDOR_OPERATIONS file '${_path}' does not exist")
    endif()
    file(SIZE "${_path}" _size)
    if(_size EQUAL 0 OR _size GREATER 131072)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_VENDOR_OPERATIONS must be 1 to 131072 bytes")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _document)
    string(REGEX MATCH "[^\t\r\n -~]" _foreign "${_document}")
    if(NOT _foreign STREQUAL "")
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_VENDOR_OPERATIONS must be printable ASCII")
    endif()
    # The manifest is written with file(GENERATE), which would evaluate a
    # generator expression and silently change the bytes the digest covers.
    string(FIND "${_document}" "$<" _generator_expression)
    if(NOT _generator_expression EQUAL -1)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_VENDOR_OPERATIONS must not contain '$<'")
    endif()
    # Tiers and descriptions are read as JSON, so text inside a description
    # can never satisfy or defeat the tier check. The broker re-validates the
    # whole declaration; these checks only make mistakes fail at configure.
    string(JSON _count ERROR_VARIABLE _json_error LENGTH "${_document}" operations)
    if(_json_error OR NOT _count GREATER 0)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_VENDOR_OPERATIONS must be a JSON object with a non-empty 'operations' array")
    endif()
    set(_used_tiers "")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE ${_last})
        string(JSON _tier ERROR_VARIABLE _tier_error GET "${_document}" operations ${_index} tier)
        string(JSON _description ERROR_VARIABLE _description_error
            GET "${_document}" operations ${_index} description)
        string(LENGTH "${_description}" _description_length)
        if(_description_error OR _description_length GREATER 512 OR
           NOT _description MATCHES "^[ -~]+$")
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): vendor operation ${_index} needs a description of one line of 1 to 512 printable ASCII characters")
        endif()
        if(NOT _tier_error)
            list(APPEND _used_tiers "${_tier}")
        endif()
    endforeach()
    foreach(_tier read gesture render destructive)
        set(_cap "dev.pulp.vendor/${_tier}@1")
        if(_tier IN_LIST _used_tiers AND NOT _cap IN_LIST control_caps)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): a '${_tier}' vendor operation requires '${_cap}' in CONTROL_CAPABILITIES")
        endif()
        if(NOT _tier IN_LIST _used_tiers AND _cap IN_LIST control_caps)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): '${_cap}' is declared without a '${_tier}' vendor operation")
        endif()
    endforeach()
    _pulp_control_json_escape(_escaped "${_document}")
    set(${output} ",\n  \"vendor_operations\": \"${_escaped}\"" PARENT_SCOPE)
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
        if(_control_cap IN_LIST _PULP_CONTROL_UNWIRED_CAPABILITIES)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): '${_control_cap}' has no product provider yet")
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
        ui.input trace.control trace.session.control state.write test.input
        authoring.tweaks runtime.reload runtime.eval sequencer.transport.loop.write
        vendor.gesture vendor.destructive)
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
        set(_support_caps
            session.describe state.read gpu.health.read diagnostics.read logs.read)
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

    _pulp_control_vendor_operations_member(_json_vendor_operations ${target} "${_control_caps}")

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
        "{\n  \"schema\": \"dev.pulp.control/artifact-manifest@1\",\n  \"schema_version\": 1,\n  \"profile\": \"${_profile}\",\n  \"target\": \"${_json_target}\",\n  \"product_name\": \"${_json_product_name}\",\n  \"bundle_id\": \"${_json_bundle_id}\",\n  \"build_id\": \"${_json_build_id}\",\n  \"registry_digest\": \"${_control_registry_digest}\",\n  \"endpoint_included\": ${_shipping},\n  \"unsafe_runtime_eval_acknowledged\": ${_runtime_eval},\n  \"permission_terms\": [\"implemented\", \"built\", \"host_available\", \"activated\", \"policy_eligible\", \"client_granted\", \"session_live\"],\n  \"capabilities\": [${_json_control_caps}]${_json_vendor_operations}\n}\n")
    file(GENERATE OUTPUT "${_manifest}" CONTENT "${_manifest_content}")
    string(SHA256 _manifest_digest "${_manifest_content}")
    set(PULP_${target}_INSPECTOR_MANIFEST "${_manifest}" CACHE INTERNAL "")

    set(PULP_${target}_CONTROL_MANIFEST_DIGEST "${_manifest_digest}" CACHE INTERNAL "" FORCE)
endfunction()

# Declare that `target`'s bytes carry a per-configure build identity, so the
# executable changes on every configure of an identical tree. Marks the target
# (PULP_COMMIT_BOUND, which catch_discover_tests turns into the `commit-bound`
# label on every test it discovers from it) and writes
# <build>/pulp-commit-bound/<target>.json, which the CI reuse record reads to
# mark the target and everything that depends on it.
function(_pulp_declare_commit_bound target)
    get_property(_declared GLOBAL PROPERTY PULP_COMMIT_BOUND_TARGETS)
    if(target IN_LIST _declared)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY PULP_COMMIT_BOUND_TARGETS "${target}")
    set_property(TARGET ${target} PROPERTY PULP_COMMIT_BOUND ON)
    # Names only: a multi-config generator (Xcode, Ninja Multi-Config) would
    # see a per-configuration artifact path as one file written with
    # different content. The codemodel maps the name to its artifacts.
    file(WRITE "${CMAKE_BINARY_DIR}/pulp-commit-bound/${target}.json"
        "{\"target\": \"${target}\"}\n")
endfunction()

# Where an artifact's control sidecars (inspector capabilities, shipping
# manifest, scan report) are written, as a generator expression. A macOS
# bundle keeps them with its resources: Apple reserves Contents/MacOS (and a
# framework's Versions/<v>) for code, and a data file there makes the bundle
# fail an ordinary, non --deep code signature such as `pulp ship sign` makes.
# Every other artifact, an iOS shallow bundle included, keeps them beside its
# binary. `out_legacy` names the directory earlier releases wrote to when it
# differs, so stale copies can be removed; it is empty otherwise.
# Readers locate them with pulp::inspect::control_sidecar_directories().
function(_pulp_control_sidecar_dir artifact_target out_dir out_legacy)
    set(_dir "$<TARGET_FILE_DIR:${artifact_target}>")
    set(_legacy "")
    if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        get_target_property(_framework ${artifact_target} FRAMEWORK)
        get_target_property(_bundle ${artifact_target} BUNDLE)
        get_target_property(_app ${artifact_target} MACOSX_BUNDLE)
        if(_framework)
            set(_legacy "${_dir}")
            set(_dir "$<TARGET_FILE_DIR:${artifact_target}>/Resources")
        elseif(_bundle OR _app)
            set(_legacy "${_dir}")
            set(_dir "$<TARGET_BUNDLE_CONTENT_DIR:${artifact_target}>/Resources")
        endif()
    endif()
    set(${out_dir} "${_dir}" PARENT_SCOPE)
    set(${out_legacy} "${_legacy}" PARENT_SCOPE)
endfunction()

function(_pulp_attach_control_shipping target artifact_target artifact_format)
    if(NOT TARGET ${artifact_target})
        message(FATAL_ERROR "control shipping target does not exist: ${artifact_target}")
    endif()
    # The marker source below embeds this build tree's control build nonce.
    _pulp_declare_commit_bound(${artifact_target})
    if(NOT artifact_format)
        message(FATAL_ERROR "control shipping format is required for ${artifact_target}")
    endif()
    # Optional: a manifest stem as the fourth argument, then
    # AFTER <file>... <target>...: steps that write into the artifact's bundle
    # (an embedded framework or extension). The persisted-stamp scan waits for
    # them and rescans when they rerun, so it never reads a bundle while it is
    # being rewritten. A file is depended on at file level; a target orders the
    # scan after it.
    cmake_parse_arguments(PARSE_ARGV 3 _pulp_cs "" "" "AFTER")
    set(_shipping_manifest_stem "${target}.${artifact_format}")
    if(_pulp_cs_UNPARSED_ARGUMENTS)
        list(GET _pulp_cs_UNPARSED_ARGUMENTS 0 _pulp_cs_stem)
        if(NOT "${_pulp_cs_stem}" STREQUAL "")
            set(_shipping_manifest_stem "${_pulp_cs_stem}")
        endif()
    endif()
    set(_after_files "")
    set(_after_targets "")
    foreach(_after IN LISTS _pulp_cs_AFTER)
        if(TARGET "${_after}")
            list(APPEND _after_targets "${_after}")
        else()
            list(APPEND _after_files "${_after}")
        endif()
    endforeach()
    string(REPLACE ";" "|" _after_inputs "${_after_files}")

    set(_profile "${PULP_${target}_CONTROL_PROFILE}")
    set(_artifact_capabilities "${PULP_${target}_CONTROL_CAPABILITIES}")
    set(_control_manifest "${PULP_${target}_INSPECTOR_MANIFEST}")
    set(_control_manifest_digest "${PULP_${target}_CONTROL_MANIFEST_DIGEST}")
    # Only Standalone composes the control endpoint today. Mixed developer
    # builds keep every plug-in module on an independent stripped declaration.
    #
    # Say so when it happens. A plug-in format reaching here with a non-stripped
    # profile means the author explicitly passed CONTROL_PROFILE to
    # pulp_add_plugin() -- the default is production-stripped, so this branch
    # cannot fire on an ordinary build. Discarding that request silently leaves
    # an author with a stripped bundle, no endpoint, and nothing in the build
    # output connecting the two; the limitation is then discoverable only by
    # reading this file. Keeping the downgrade and announcing it preserves the
    # safe default for shipped artifacts while making the boundary visible at
    # the moment it bites.
    if(NOT artifact_format STREQUAL "Standalone" AND
       NOT _profile STREQUAL "production-stripped")
        message(WARNING
            "pulp_add_plugin(${target}): CONTROL_PROFILE '${_profile}' is not composed for "
            "${artifact_format}; only Standalone composes the control endpoint today. This "
            "${artifact_format} artifact ships a production-stripped declaration with no "
            "capabilities and endpoint_included=false. A plug-in module is loaded into a host "
            "process the author does not own, so the endpoint is not offered there by default.")
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
        if(APPLE AND CMAKE_OSX_ARCHITECTURES)
            set(_architectures "${CMAKE_OSX_ARCHITECTURES}")
        elseif(WIN32)
            include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpWindowsTargetArch.cmake")
            pulp_windows_target_arch(_architectures)
        else()
            set(_architectures "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
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
        "${_stamp_dir}/${artifact_target}.${_format_identifier}.$<CONFIG>.sidecars-v2.stamp")
    set(_shipping_manifest
        "${_shipping_dir}/${_shipping_manifest_stem}.control-shipping.json")
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
        "#if defined(_MSC_VER)\n#define PULP_SHIPPING_USED __declspec(dllexport)\n#else\n#define PULP_SHIPPING_USED __attribute__((used, visibility(\"default\")))\n#endif\nextern \"C\" PULP_SHIPPING_USED const volatile char pulp_control_shipping_${_format_identifier}_v1[] = \"PULP_CONTROL_PROFILE_${_profile_identifier}_V1\\0PULP_CONTROL_MANIFEST_SHA256_${_control_manifest_digest}_V1\\0PULP_CONTROL_SHIPPING_SHA256_${_shipping_manifest_digest}_V1\\0PULP_CONTROL_ARTIFACT_FORMAT_${_format_identifier}_V1\\0PULP_CONTROL_ARTIFACT_PLATFORM_${_platform_identifier}_V1${_architecture_markers}${_standalone_marker}\";\n")
    file(GENERATE OUTPUT "${_marker_source}" CONTENT "${_marker_content}")
    target_sources(${artifact_target} PRIVATE "${_marker_source}")

    _pulp_control_sidecar_dir(${artifact_target} _sidecar_dir _legacy_sidecar_dir)
    set(_legacy_sidecar_removal "")
    if(_legacy_sidecar_dir)
        # A bundle built before the sidecars moved out of the code directory
        # keeps the old copies across incremental builds, and they alone make
        # it unsignable; remove them.
        set(_legacy_sidecar_removal
            COMMAND "${CMAKE_COMMAND}" -E rm -f
                "${_legacy_sidecar_dir}/${target}.inspector-capabilities.json"
                "${_legacy_sidecar_dir}/${_shipping_manifest_stem}.control-shipping.json"
                "${_legacy_sidecar_dir}/${_shipping_manifest_stem}.control-shipping-report.json")
    endif()
    # A dev-shared module (PULP_DEV_SHARED_UI) cannot ship, so a direct build
    # of it skips this step and only the persisted-stamp edge below scans it.
    get_target_property(_dev_shared ${artifact_target} PULP_DEV_SHARED_UI)
    if(NOT _dev_shared)
        add_custom_command(TARGET ${artifact_target} POST_BUILD
            ${_legacy_sidecar_removal}
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sidecar_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_control_manifest}"
                "${_sidecar_dir}/${target}.inspector-capabilities.json"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_shipping_manifest}"
                "${_sidecar_dir}/${_shipping_manifest_stem}.control-shipping.json"
            COMMAND "${CMAKE_COMMAND}"
                -DARTIFACT=$<TARGET_FILE:${artifact_target}>
                -DMANIFEST=${_control_manifest}
                -DSHIPPING_MANIFEST=${_shipping_manifest}
                -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DREPORT=${_sidecar_dir}/${_shipping_manifest_stem}.control-shipping-report.json
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_control_shipping_artifact.cmake"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_scan_stamp}"
            VERBATIM)
    endif()

    # POST_BUILD protects direct builds of the artifact. The persisted stamp
    # additionally makes scanner/manifests first-class dependencies, so an
    # install or normal ALL build re-scans an unchanged binary after policy
    # changes instead of trusting a stale report.
    #
    # When the artifact relinks in the same build, the generator has already
    # scheduled this stamp edge, but POST_BUILD has just scanned the new binary
    # and touched the stamp. SKIP_IF_FRESH_STAMP lets the scanner return early
    # when the stamp is strictly newer than every input this edge depends on,
    # instead of scanning the same bytes a second time.
    # The scanner reads the shared forbidden-marker list through its module;
    # both are policy inputs of the stamp edge, as they are of the scanner's
    # own SKIP_IF_FRESH_STAMP check.
    set(_scanner "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_control_shipping_artifact.cmake")
    set(_scan_target "${artifact_target}_ControlShippingScan_${_format_identifier}")
    # The stamp's name carries the sidecar layout, so a build tree from before
    # the layout changed has no such stamp and re-runs this step even when no
    # relink follows (Make and Xcode do not relink for a changed POST_BUILD
    # command); it moves the sidecars the same way the POST_BUILD step does.
    add_custom_command(
        OUTPUT "${_scan_stamp}"
        ${_legacy_sidecar_removal}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sidecar_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_control_manifest}"
            "${_sidecar_dir}/${target}.inspector-capabilities.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_shipping_manifest}"
            "${_sidecar_dir}/${_shipping_manifest_stem}.control-shipping.json"
        COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=$<TARGET_FILE:${artifact_target}>
            -DMANIFEST=${_control_manifest}
            -DSHIPPING_MANIFEST=${_shipping_manifest}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DREPORT=${_sidecar_dir}/${_shipping_manifest_stem}.control-shipping-report.json
            -DSKIP_IF_FRESH_STAMP=${_scan_stamp}
            "-DSCAN_AFTER_INPUTS=${_after_inputs}"
            -P "${_scanner}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_scan_stamp}"
        DEPENDS ${artifact_target} "${_control_manifest}"
            "${_shipping_manifest}" "${_scanner}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpReleaseArtifactScan.cmake"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/release_artifact_forbidden.json"
            ${_after_files}
        VERBATIM)
    add_custom_target(${_scan_target} ALL DEPENDS "${_scan_stamp}")
    if(_after_targets)
        add_dependencies(${_scan_target} ${_after_targets})
    endif()
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
