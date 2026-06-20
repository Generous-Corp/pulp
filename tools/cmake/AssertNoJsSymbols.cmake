# AssertNoJsSymbols.cmake — build-time guard for native-only (PULP_ENABLE_JS=OFF)
# builds. Fails the build if a binary that must be JS-free has linked the
# pulp::view-script scripting layer (JS engine + web-compat + widget bridge).
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

# pulp::view-script symbols — present iff the JS engine + widget bridge were
# linked. Engine-agnostic: ScriptEngine/WidgetBridge/ScriptedUI are Pulp's own
# classes (mangled names embed these substrings), and JS_NewRuntime/JS_NewContext
# are QuickJS's C API. Any of them in a PULP_ENABLE_JS=OFF binary is a leak.
set(_js_markers ScriptEngine WidgetBridge ScriptedUI JS_NewRuntime JS_NewContext)
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
