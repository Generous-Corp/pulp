#[[
Renders the macOS AUv3 entitlement templates the way PulpAuv3.cmake does, for
each JS engine and both library-validation postures, and asserts the resulting
entitlement set. Runs under `cmake -P` so every host checks the policy without
an Apple toolchain or a full configure.
]]

cmake_minimum_required(VERSION 3.22)

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_repo_root}/tools/cmake/PulpAuv3Entitlements.cmake")
set(_template_dir "${_repo_root}/tools/templates/auv3")
set(_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/auv3-entitlement-probes")
file(MAKE_DIRECTORY "${_probe_dir}")

set(_failures "")

function(_render engine disable_library_validation template out_var)
    pulp_auv3_entitlement_fragments("${engine}" "${disable_library_validation}")
    set(_out "${_probe_dir}/${engine}-${disable_library_validation}-${template}.entitlements")
    configure_file("${_template_dir}/${template}" "${_out}" @ONLY)
    file(READ "${_out}" _text)
    set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()

# Assert `key` is present (expected=ON) or absent (expected=OFF) as an enabled
# entitlement in `text`.
function(_expect label text key expected)
    set(_pattern "<key>${key}</key>[ \t\r\n]*<true/>")
    string(REGEX MATCH "${_pattern}" _hit "${text}")
    if(expected AND NOT _hit)
        set(_failures "${_failures};${label}: expected ${key} to be granted" PARENT_SCOPE)
    elseif(NOT expected AND _hit)
        set(_failures "${_failures};${label}: ${key} must not be granted" PARENT_SCOPE)
    endif()
endfunction()

set(_jit "com.apple.security.cs.allow-jit")
set(_unsigned "com.apple.security.cs.allow-unsigned-executable-memory")
set(_libval "com.apple.security.cs.disable-library-validation")
set(_sandbox "com.apple.security.app-sandbox")

foreach(_template Appex-Entitlements.plist.template Host-Entitlements.plist.template)
    # Interpreter-only engines: no JIT entitlement of any kind.
    foreach(_engine auto quickjs)
        _render("${_engine}" OFF "${_template}" _text)
        _expect("${_template}/${_engine}" "${_text}" "${_jit}" OFF)
        _expect("${_template}/${_engine}" "${_text}" "${_unsigned}" OFF)
        _expect("${_template}/${_engine}" "${_text}" "${_libval}" OFF)
    endforeach()
    # JIT engines: allow-jit only; MAP_JIT covers V8 and JSC, so the
    # unsigned-executable-memory entitlement is never emitted.
    foreach(_engine v8 jsc)
        _render("${_engine}" OFF "${_template}" _text)
        _expect("${_template}/${_engine}" "${_text}" "${_jit}" ON)
        _expect("${_template}/${_engine}" "${_text}" "${_unsigned}" OFF)
        _expect("${_template}/${_engine}" "${_text}" "${_libval}" OFF)
    endforeach()
    # Library validation is disabled only on explicit request.
    _render(quickjs ON "${_template}" _text)
    _expect("${_template}/quickjs+libval" "${_text}" "${_libval}" ON)
    _expect("${_template}/quickjs+libval" "${_text}" "${_jit}" OFF)

    # No placeholder survives rendering and the dict stays well formed.
    _render(v8 ON "${_template}" _text)
    if(_text MATCHES "@PULP_AUV3_")
        list(APPEND _failures "${_template}: unexpanded placeholder")
    endif()
    string(REGEX MATCHALL "<key>" _keys "${_text}")
    string(REGEX MATCHALL "<true/>" _trues "${_text}")
    list(LENGTH _keys _nkeys)
    list(LENGTH _trues _ntrues)
    if(NOT _nkeys EQUAL _ntrues)
        list(APPEND _failures "${_template}: ${_nkeys} keys but ${_ntrues} values")
    endif()
    if(NOT _text MATCHES "<dict>[^@]*</dict>")
        list(APPEND _failures "${_template}: dict is not closed")
    endif()
endforeach()

# The .appex must stay sandboxed whatever the engine.
_render(v8 OFF Appex-Entitlements.plist.template _appex)
_expect("Appex/sandbox" "${_appex}" "${_sandbox}" ON)
_render(v8 OFF Host-Entitlements.plist.template _host)
_expect("Host/no-sandbox" "${_host}" "${_sandbox}" OFF)

# Control: the matcher itself sees a granted key when one is present.
_expect("control" "<key>${_jit}</key>\n<true/>" "${_jit}" ON)
_expect("control-absent" "<key>other</key>\n<true/>" "${_jit}" OFF)

file(REMOVE_RECURSE "${_probe_dir}")

list(FILTER _failures EXCLUDE REGEX "^$")
if(_failures)
    string(REPLACE ";" "\n  " _report "${_failures}")
    message(FATAL_ERROR "AUv3 entitlement policy violated:\n  ${_report}")
endif()
message(STATUS "AUv3 entitlement templates render the expected set for every engine")
