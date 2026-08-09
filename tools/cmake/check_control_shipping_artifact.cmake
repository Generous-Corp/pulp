if(NOT EXISTS "${ARTIFACT}" OR NOT EXISTS "${MANIFEST}" OR
   NOT EXISTS "${SHIPPING_MANIFEST}")
    message(FATAL_ERROR
        "control shipping scan requires ARTIFACT, MANIFEST, and SHIPPING_MANIFEST")
endif()

file(READ "${ARTIFACT}" _binary HEX)
file(READ "${MANIFEST}" _manifest)
file(READ "${SHIPPING_MANIFEST}" _shipping_manifest)
file(SIZE "${ARTIFACT}" _artifact_size)

function(_pulp_require_hex_marker marker reason)
    string(HEX "${marker}" _marker_hex)
    string(FIND "${_binary}" "${_marker_hex}" _marker_position)
    if(_marker_position LESS 0)
        message(FATAL_ERROR "${reason}: ${marker}")
    endif()
endfunction()

function(_pulp_forbid_hex_marker marker reason)
    string(HEX "${marker}" _marker_hex)
    string(FIND "${_binary}" "${_marker_hex}" _marker_position)
    if(_marker_position GREATER_EQUAL 0)
        message(FATAL_ERROR "${reason}: ${marker}")
    endif()
endfunction()

# Extract dependency entries from the native loader scanner instead of treating
# arbitrary substrings in its prose (including the artifact path header) as a
# dependency.  This is deliberately basename-based because packaged siblings
# are resolved by filename, while otool/readelf may report absolute, @rpath, or
# SONAME-style paths.
function(_pulp_loader_names_dependency dependency_name out_var)
    string(REPLACE "\r\n" "\n" _dependency_lines "${_dependency_output}")
    string(REPLACE "\r" "\n" _dependency_lines "${_dependency_lines}")
    string(REPLACE "\n" ";" _dependency_lines "${_dependency_lines}")
    set(_dependency_found false)
    set(_dumpbin_dependency_section false)
    foreach(_dependency_line IN LISTS _dependency_lines)
        string(STRIP "${_dependency_line}" _dependency_line)
        set(_loader_entry "")
        if(_dependency_scanner STREQUAL "otool")
            if(_dependency_line MATCHES "^(.+)[ \t]+\\(compatibility version")
                set(_loader_entry "${CMAKE_MATCH_1}")
            endif()
        elseif(_dependency_scanner STREQUAL "readelf")
            if(_dependency_line MATCHES "\\(NEEDED\\).*[[]([^]]+)[]]")
                set(_loader_entry "${CMAKE_MATCH_1}")
            endif()
        elseif(_dependency_scanner STREQUAL "dumpbin")
            if(_dependency_line MATCHES
               "^Image has the following (delay load )?dependencies:$")
                set(_dumpbin_dependency_section true)
                continue()
            elseif(_dumpbin_dependency_section AND
                   _dependency_line STREQUAL "Summary")
                set(_dumpbin_dependency_section false)
            elseif(_dumpbin_dependency_section AND _dependency_line)
                set(_loader_entry "${_dependency_line}")
            endif()
        endif()
        if(NOT _loader_entry)
            continue()
        endif()
        get_filename_component(_loader_name "${_loader_entry}" NAME)
        if(WIN32)
            string(TOLOWER "${_loader_name}" _loader_name)
            string(TOLOWER "${dependency_name}" _dependency_name)
        else()
            set(_dependency_name "${dependency_name}")
        endif()
        if(_loader_name STREQUAL _dependency_name)
            set(_dependency_found true)
            break()
        endif()
    endforeach()
    set(${out_var} "${_dependency_found}" PARENT_SCOPE)
endfunction()

string(FIND "${_manifest}"
    "\"schema\": \"dev.pulp.control/artifact-manifest@1\"" _control_schema)
string(FIND "${_shipping_manifest}"
    "\"schema\": \"dev.pulp.control/shipping-artifact@1\"" _shipping_schema)
if(_control_schema LESS 0 OR _shipping_schema LESS 0)
    message(FATAL_ERROR "unsupported control shipping manifest schema")
endif()

foreach(_field IN ITEMS profile format platform)
    string(REGEX MATCH "\"${_field}\"[ \t]*:[ \t]*\"([^\"]+)\""
        _field_match "${_shipping_manifest}")
    if(NOT _field_match)
        message(FATAL_ERROR "control shipping manifest is missing ${_field}")
    endif()
    set(_${_field} "${CMAKE_MATCH_1}")
endforeach()

string(REGEX MATCH
    "\"control_manifest_sha256\"[ \t]*:[ \t]*\"([0-9a-f]+)\""
    _digest_match "${_shipping_manifest}")
set(_control_digest "${CMAKE_MATCH_1}")
string(LENGTH "${_control_digest}" _control_digest_length)
if(NOT _control_digest_length EQUAL 64)
    message(FATAL_ERROR "control shipping manifest has an invalid control digest")
endif()
string(SHA256 _actual_control_digest "${_manifest}")
if(NOT _actual_control_digest STREQUAL _control_digest)
    message(FATAL_ERROR "shipping manifest names a stale control manifest digest")
endif()
string(SHA256 _shipping_digest "${_shipping_manifest}")

string(MAKE_C_IDENTIFIER "${_profile}" _profile_identifier)
string(TOUPPER "${_profile_identifier}" _profile_identifier)
string(MAKE_C_IDENTIFIER "${_format}" _format_identifier)
string(TOUPPER "${_format_identifier}" _format_identifier)
string(MAKE_C_IDENTIFIER "${_platform}" _platform_identifier)
string(TOUPPER "${_platform_identifier}" _platform_identifier)
_pulp_require_hex_marker("PULP_CONTROL_PROFILE_${_profile_identifier}_V1"
    "declared control profile marker is absent")
_pulp_require_hex_marker("PULP_CONTROL_MANIFEST_SHA256_${_control_digest}_V1"
    "control manifest digest marker is absent or stale")
_pulp_require_hex_marker("PULP_CONTROL_SHIPPING_SHA256_${_shipping_digest}_V1"
    "shipping manifest digest marker is absent or stale")
_pulp_require_hex_marker("PULP_CONTROL_ARTIFACT_FORMAT_${_format_identifier}_V1"
    "artifact format marker is absent")
_pulp_require_hex_marker("PULP_CONTROL_ARTIFACT_PLATFORM_${_platform_identifier}_V1"
    "artifact platform marker is absent")

string(REGEX MATCH
    "\"architectures\"[ \t]*:[ \t]*[[]([^]]+)[]]"
    _architectures_match "${_shipping_manifest}")
if(NOT _architectures_match)
    message(FATAL_ERROR "control shipping manifest has no architectures")
endif()
set(_architectures_text "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "\"[^\"]+\"" _architecture_tokens "${_architectures_text}")
set(_architectures "")
foreach(_architecture_token IN LISTS _architecture_tokens)
    string(REPLACE "\"" "" _architecture "${_architecture_token}")
    list(APPEND _architectures "${_architecture}")
    string(MAKE_C_IDENTIFIER "${_architecture}" _architecture_identifier)
    string(TOUPPER "${_architecture_identifier}" _architecture_identifier)
    _pulp_require_hex_marker("PULP_CONTROL_ARTIFACT_ARCH_${_architecture_identifier}_V1"
        "artifact architecture marker is absent")
endforeach()

set(_dependency_output "")
set(_symbol_output "")
set(_dependency_scanner "unavailable")
set(_symbol_scanner "unavailable")
set(_architecture_scanner "unavailable")
if(APPLE)
    find_program(_pulp_otool NAMES otool)
    find_program(_pulp_nm NAMES nm llvm-nm)
    find_program(_pulp_lipo NAMES lipo)
    if(_pulp_otool)
        execute_process(COMMAND "${_pulp_otool}" -L "${ARTIFACT}"
            RESULT_VARIABLE _dependency_result OUTPUT_VARIABLE _dependency_output
            ERROR_VARIABLE _dependency_error)
        if(NOT _dependency_result EQUAL 0)
            message(FATAL_ERROR "otool dependency scan failed: ${_dependency_error}")
        endif()
        set(_dependency_scanner "otool")
    endif()
    if(_pulp_nm)
        execute_process(COMMAND "${_pulp_nm}" -C "${ARTIFACT}"
            RESULT_VARIABLE _symbol_result OUTPUT_VARIABLE _symbol_output
            ERROR_VARIABLE _symbol_error)
        if(NOT _symbol_result EQUAL 0)
            message(FATAL_ERROR "nm symbol scan failed: ${_symbol_error}")
        endif()
        set(_symbol_scanner "nm")
    endif()
    if(_pulp_lipo)
        execute_process(COMMAND "${_pulp_lipo}" -archs "${ARTIFACT}"
            RESULT_VARIABLE _arch_result OUTPUT_VARIABLE _actual_architectures
            ERROR_VARIABLE _arch_error OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _arch_result EQUAL 0)
            message(FATAL_ERROR "lipo architecture scan failed: ${_arch_error}")
        endif()
        foreach(_architecture IN LISTS _architectures)
            string(FIND " ${_actual_architectures} " " ${_architecture} " _arch_position)
            if(_arch_position LESS 0)
                message(FATAL_ERROR
                    "declared architecture is absent from artifact: ${_architecture}")
            endif()
        endforeach()
        set(_architecture_scanner "lipo")
    endif()
elseif(UNIX)
    find_program(_pulp_readelf NAMES readelf llvm-readelf)
    find_program(_pulp_nm NAMES nm llvm-nm)
    if(_pulp_readelf)
        execute_process(COMMAND "${_pulp_readelf}" -d "${ARTIFACT}"
            RESULT_VARIABLE _dependency_result OUTPUT_VARIABLE _dependency_output
            ERROR_VARIABLE _dependency_error)
        if(NOT _dependency_result EQUAL 0)
            message(FATAL_ERROR "readelf dependency scan failed: ${_dependency_error}")
        endif()
        set(_dependency_scanner "readelf")
        execute_process(COMMAND "${_pulp_readelf}" -h "${ARTIFACT}"
            RESULT_VARIABLE _arch_result OUTPUT_VARIABLE _actual_architectures
            ERROR_VARIABLE _arch_error)
        if(NOT _arch_result EQUAL 0)
            message(FATAL_ERROR "readelf architecture scan failed: ${_arch_error}")
        endif()
        set(_architecture_scanner "readelf")
    endif()
    if(_pulp_nm)
        execute_process(COMMAND "${_pulp_nm}" -C "${ARTIFACT}"
            RESULT_VARIABLE _symbol_result OUTPUT_VARIABLE _symbol_output
            ERROR_VARIABLE _symbol_error)
        if(NOT _symbol_result EQUAL 0)
            message(FATAL_ERROR "nm symbol scan failed: ${_symbol_error}")
        endif()
        set(_symbol_scanner "nm")
    endif()
elseif(WIN32)
    set(_compiler_hints "")
    if(CXX_COMPILER)
        get_filename_component(_compiler_dir "${CXX_COMPILER}" DIRECTORY)
        list(APPEND _compiler_hints "${_compiler_dir}")
    endif()
    find_program(_pulp_dumpbin NAMES dumpbin.exe dumpbin HINTS ${_compiler_hints})
    if(_pulp_dumpbin)
        execute_process(COMMAND "${_pulp_dumpbin}" /DEPENDENTS "${ARTIFACT}"
            RESULT_VARIABLE _dependency_result OUTPUT_VARIABLE _dependency_output
            ERROR_VARIABLE _dependency_error)
        if(NOT _dependency_result EQUAL 0)
            message(FATAL_ERROR "dumpbin dependency scan failed: ${_dependency_error}")
        endif()
        execute_process(COMMAND "${_pulp_dumpbin}" /SYMBOLS /UNDNAME "${ARTIFACT}"
            RESULT_VARIABLE _symbol_result OUTPUT_VARIABLE _symbol_output
            ERROR_VARIABLE _symbol_error)
        if(NOT _symbol_result EQUAL 0)
            message(FATAL_ERROR "dumpbin symbol scan failed: ${_symbol_error}")
        endif()
        set(_dependency_scanner "dumpbin")
        set(_symbol_scanner "dumpbin")
        execute_process(COMMAND "${_pulp_dumpbin}" /HEADERS "${ARTIFACT}"
            RESULT_VARIABLE _arch_result OUTPUT_VARIABLE _actual_architectures
            ERROR_VARIABLE _arch_error)
        if(NOT _arch_result EQUAL 0)
            message(FATAL_ERROR "dumpbin architecture scan failed: ${_arch_error}")
        endif()
        set(_architecture_scanner "dumpbin")
    endif()
endif()

if(_symbol_scanner STREQUAL "unavailable" OR
   _dependency_scanner STREQUAL "unavailable" OR
   _architecture_scanner STREQUAL "unavailable")
    message(FATAL_ERROR
        "control shipping requires native symbol, dependency, and architecture scanners")
endif()

if(NOT APPLE)
    string(TOLOWER "${_actual_architectures}" _actual_architectures_lower)
    foreach(_architecture IN LISTS _architectures)
        string(TOLOWER "${_architecture}" _declared_architecture_lower)
        if(_declared_architecture_lower MATCHES "^(x86_64|amd64|x64)$")
            if(NOT _actual_architectures_lower MATCHES "x86-64|x86_64|amd64|8664")
                message(FATAL_ERROR
                    "declared architecture is absent from artifact: ${_architecture}")
            endif()
        elseif(_declared_architecture_lower MATCHES "^(arm64|aarch64)$")
            if(NOT _actual_architectures_lower MATCHES "arm64|aarch64|aa64")
                message(FATAL_ERROR
                    "declared architecture is absent from artifact: ${_architecture}")
            endif()
        elseif(_declared_architecture_lower MATCHES "^arm")
            if(NOT _actual_architectures_lower MATCHES "arm")
                message(FATAL_ERROR
                    "declared architecture is absent from artifact: ${_architecture}")
            endif()
        endif()
    endforeach()
endif()

set(_closure_binary_count 0)
if(_profile STREQUAL "production-stripped")
    foreach(_forbidden_marker IN ITEMS
            PULP_INSPECT_SHIPPING_MANIFEST_V1
            PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1
            PULP_REMOTE_VIEW_PARAMETER_AUTHORITY_V1
            PULP_INSPECT_CAPABILITY_
            view.param_set)
        _pulp_forbid_hex_marker("${_forbidden_marker}"
            "production-stripped artifact contains forbidden control string")
    endforeach()
    # Match canonical control implementation namespaces. Generic names such as
    # ControlService or RemoteView are valid names for ordinary plugin code and
    # are not evidence that a control implementation leaked into the artifact.
    foreach(_forbidden_symbol IN ITEMS
            "pulp::inspect::ControlBroker"
            "pulp::inspect::ControlService"
            "pulp::inspect::ControlEndpoint"
            "pulp::inspect::RuntimeEvaluator")
        string(FIND "${_symbol_output}" "${_forbidden_symbol}" _symbol_position)
        if(_symbol_position GREATER_EQUAL 0)
            message(FATAL_ERROR
                "production-stripped artifact contains forbidden control symbol: ${_forbidden_symbol}")
        endif()
    endforeach()
    foreach(_forbidden_dependency IN ITEMS
            pulp-inspect-control pulp-inspect-client pulp-inspect-runtime
            pulp-inspect-runtime-eval pulp-inspect-standalone-runtime
            pulp-control-broker)
        string(FIND "${_dependency_output}" "${_forbidden_dependency}" _dependency_position)
        if(_dependency_position GREATER_EQUAL 0)
            message(FATAL_ERROR
                "production-stripped artifact links forbidden dependency: ${_forbidden_dependency}")
        endif()
    endforeach()

    # A renamed helper library must not hide control authority. Scan every
    # native binary packaged inside a bundle, and every sibling binary named
    # by the loader dependency table for unbundled outputs.
    get_filename_component(_artifact_directory "${ARTIFACT}" DIRECTORY)
    set(_closure_root "${_artifact_directory}")
    set(_bundle_closure false)
    if("${ARTIFACT}" MATCHES
       "^(.+\\.(app|vst3|clap|component|appex|aaxplugin|lv2))(/.*)?$")
        set(_closure_root "${CMAKE_MATCH_1}")
        set(_bundle_closure true)
    endif()
    file(GLOB_RECURSE _closure_candidates LIST_DIRECTORIES false
        "${_closure_root}/*")
    foreach(_closure_candidate IN LISTS _closure_candidates)
        if("${_closure_candidate}" STREQUAL "${ARTIFACT}")
            continue()
        endif()
        get_filename_component(_closure_name "${_closure_candidate}" NAME)
        if(NOT _bundle_closure)
            _pulp_loader_names_dependency("${_closure_name}"
                _closure_is_dependency)
            if(NOT _closure_is_dependency)
                continue()
            endif()
        endif()
        file(READ "${_closure_candidate}" _closure_magic LIMIT 4 HEX)
        string(TOLOWER "${_closure_magic}" _closure_magic)
        if(NOT _closure_magic MATCHES
           "^(7f454c46|4d5a|cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca)$")
            continue()
        endif()
        math(EXPR _closure_binary_count "${_closure_binary_count} + 1")
        file(READ "${_closure_candidate}" _closure_binary HEX)
        foreach(_forbidden_marker IN ITEMS
                PULP_INSPECT_SHIPPING_MANIFEST_V1
                PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1
                PULP_REMOTE_VIEW_PARAMETER_AUTHORITY_V1
                PULP_INSPECT_CAPABILITY_
                view.param_set)
            string(HEX "${_forbidden_marker}" _closure_marker_hex)
            string(FIND "${_closure_binary}" "${_closure_marker_hex}"
                _closure_marker_position)
            if(_closure_marker_position GREATER_EQUAL 0)
                message(FATAL_ERROR
                    "production-stripped artifact contains forbidden control string in packaged dependency ${_closure_name}: ${_forbidden_marker}")
            endif()
        endforeach()
        if(WIN32)
            execute_process(COMMAND "${_pulp_dumpbin}" /SYMBOLS /UNDNAME
                "${_closure_candidate}" RESULT_VARIABLE _closure_symbol_result
                OUTPUT_VARIABLE _closure_symbols ERROR_VARIABLE _closure_symbol_error)
        else()
            execute_process(COMMAND "${_pulp_nm}" -C "${_closure_candidate}"
                RESULT_VARIABLE _closure_symbol_result
                OUTPUT_VARIABLE _closure_symbols ERROR_VARIABLE _closure_symbol_error)
        endif()
        # Fully stripped libraries can have no printable symbols; their
        # implementation-owned markers above remain fail-closed evidence.
        if(_closure_symbol_result EQUAL 0)
            foreach(_forbidden_symbol IN ITEMS
                    "pulp::inspect::ControlBroker"
                    "pulp::inspect::ControlService"
                    "pulp::inspect::ControlEndpoint"
                    "pulp::inspect::RuntimeEvaluator")
                string(FIND "${_closure_symbols}" "${_forbidden_symbol}"
                    _closure_symbol_position)
                if(_closure_symbol_position GREATER_EQUAL 0)
                    message(FATAL_ERROR
                        "production-stripped artifact contains forbidden control symbol in packaged dependency ${_closure_name}: ${_forbidden_symbol}")
                endif()
            endforeach()
        endif()
    endforeach()
else()
    _pulp_require_hex_marker("PULP_INSPECT_SHIPPING_MANIFEST_V1"
        "control-enabled profile has no retained endpoint marker")
    set(_capability_contracts
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
    set(_capability_markers
        SESSION_DESCRIBE SESSION_CONTROL STATE_READ UI_READ DIAGNOSTICS_READ
        LOGS_READ CAPTURE_IMAGE STATE_WRITE TEST_INPUT AUTHORING_TWEAKS
        TELEMETRY_STREAM RUNTIME_EVAL)
    set(_declared_marker_count 0)
    list(LENGTH _capability_contracts _capability_contract_count)
    math(EXPR _capability_contract_last "${_capability_contract_count} - 1")
    foreach(_capability_index RANGE ${_capability_contract_last})
        list(GET _capability_contracts ${_capability_index} _capability_contract)
        list(GET _capability_markers ${_capability_index} _capability_marker)
        string(FIND "${_manifest}" "\"${_capability_contract}\"" _declared_position)
        string(HEX "PULP_INSPECT_CAPABILITY_${_capability_marker}_V1" _marker_hex)
        string(FIND "${_binary}" "${_marker_hex}" _binary_position)
        if(_declared_position GREATER_EQUAL 0 AND _binary_position LESS 0)
            message(FATAL_ERROR
                "declared capability marker is absent: ${_capability_contract}")
        elseif(_declared_position GREATER_EQUAL 0)
            math(EXPR _declared_marker_count "${_declared_marker_count} + 1")
        endif()
    endforeach()
    if(_declared_marker_count EQUAL 0)
        message(FATAL_ERROR "control-enabled profile has no declared capability marker")
    endif()
endif()

string(FIND "${_manifest}" "\"unsafe_runtime_eval_acknowledged\": true" _eval_declared)
string(HEX "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1" _eval_hex)
string(FIND "${_binary}" "${_eval_hex}" _eval_binary)
if(_eval_declared GREATER_EQUAL 0 AND _eval_binary LESS 0)
    message(FATAL_ERROR
        "runtime.eval acknowledgement is present but high-risk component marker is absent")
elseif(_eval_declared LESS 0 AND _eval_binary GREATER_EQUAL 0)
    message(FATAL_ERROR
        "high-risk runtime.eval component is present without its separate acknowledgement")
endif()

if(REPORT)
    string(REPLACE "\\" "\\\\" _artifact_json "${ARTIFACT}")
    string(REPLACE "\"" "\\\"" _artifact_json "${_artifact_json}")
    file(WRITE "${REPORT}"
        "{\n  \"schema\": \"dev.pulp.control/shipping-report@1\",\n  \"artifact\": \"${_artifact_json}\",\n  \"artifact_size_bytes\": ${_artifact_size},\n  \"profile\": \"${_profile}\",\n  \"format\": \"${_format}\",\n  \"platform\": \"${_platform}\",\n  \"architectures\": [${_architectures_text}],\n  \"symbol_scanner\": \"${_symbol_scanner}\",\n  \"dependency_scanner\": \"${_dependency_scanner}\",\n  \"architecture_scanner\": \"${_architecture_scanner}\",\n  \"packaged_dependency_binaries_scanned\": ${_closure_binary_count},\n  \"verdict\": \"pass\"\n}\n")
endif()
