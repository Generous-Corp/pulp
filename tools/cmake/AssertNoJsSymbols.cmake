# AssertNoJsSymbols.cmake — BEST-EFFORT secondary native-only guard.
#
# IMPORTANT: this binary symbol-scan is NOT authoritative. pulp::view-script is a
# static archive, so the linker drops any member whose symbols aren't referenced
# — these markers can be absent from the final binary even when the archive WAS
# linked, so the scan cannot reliably catch a stray link. The reliable guard is
# the configure-time LINK_LIBRARIES assertion in core/view/CMakeLists.txt
# (PULP_ENABLE_JS=OFF branch). This check is kept only as defence-in-depth: it
# still catches a JS-layer symbol force-referenced / whole-archived into a binary.
# Mirrors the MacGpuWindowHost nm-check convention used elsewhere in the tree.
#
# Invoked as a POST_BUILD step:
#   cmake -DBIN=<path-to-binary> -P tools/cmake/AssertNoJsSymbols.cmake
if(NOT DEFINED BIN)
    message(FATAL_ERROR "AssertNoJsSymbols: BIN not set")
endif()

find_program(NM_EXE nm)
if(NOT NM_EXE)
    message(WARNING "AssertNoJsSymbols: 'nm' not found; skipping native-only symbol check for ${BIN}")
    return()
endif()

execute_process(COMMAND "${NM_EXE}" "${BIN}" OUTPUT_VARIABLE _syms ERROR_QUIET)

# pulp::view-script symbols. Engine-agnostic: ScriptEngine/WidgetBridge/ScriptedUi
# are Pulp's own classes (mangled names embed these substrings), and
# JS_NewRuntime/JS_NewContext are QuickJS's C API. Any of them in a
# PULP_ENABLE_JS=OFF binary indicates a JS-layer leak that survived dead-strip.
# (ScriptedUi: the class is ScriptedUiSession/ScriptedUiOptions — lowercase 'i'.)
set(_js_markers ScriptEngine WidgetBridge ScriptedUi JS_NewRuntime JS_NewContext)
set(_found "")
foreach(_m IN LISTS _js_markers)
    string(FIND "${_syms}" "${_m}" _idx)
    if(NOT _idx EQUAL -1)
        list(APPEND _found "${_m}")
    endif()
endforeach()

if(_found)
    message(FATAL_ERROR
        "Native-only build (PULP_ENABLE_JS=OFF) leaked JS-layer symbols into\n"
        "  ${BIN}\n"
        "  found: ${_found}\n"
        "The pulp::view-script (JS engine + web-compat) layer must NOT be linked "
        "when PULP_ENABLE_JS=OFF. Check the pulp-view link in "
        "core/view/CMakeLists.txt.")
endif()

# Confirm the native Skia-SVG render path IS present (the faithful-vector lane
# the native-only build is meant to keep). A warning, not a hard failure — a
# trivial native binary may legitimately not pull SkSVGDOM.
string(FIND "${_syms}" "SkSVG" _sk)
if(_sk EQUAL -1)
    message(WARNING
        "AssertNoJsSymbols: no SkSVG* symbols in ${BIN} — the native Skia-SVG "
        "design-import path may not be linked into this binary.")
endif()

message(STATUS "Native-only symbol check passed (no JS-layer symbols): ${BIN}")
