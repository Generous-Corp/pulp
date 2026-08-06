# Embed JS preludes as C++ string constants.
set(PULP_JS_PRELUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
    # Events + pointer-capture prototype overrides. Loaded AFTER the
    # parent so the Element constructor + prototype are already defined
    # when the overrides install.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element-events.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas.js
    # Native GPU helpers. Loaded AFTER the parent so the
    # CanvasRenderingContext2D constructor + window.pulp.gpu surface are
    # in scope when GPU consumers call getContext("webgpu").
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-gpu.js
    # _PulpCanvasMatrix DOMMatrix-compat helper.
    # CanvasRenderingContext2D.getTransform() instantiates this lazily
    # at call time so embed order relative to canvas.js is flexible.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-matrix.js
    # Canvas2D API coverage methods:
    # measureText / drawImage / setLineDash / getLineDash /
    # getImageData / putImageData. Loaded AFTER the parent so the
    # CanvasRenderingContext2D constructor is in scope.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-image.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
    # Per-domain `_applyProperty` handler modules. Layout / paint /
    # typography / transform / misc handlers stay split by ownership;
    # web-compat-style-decl.js's `_applyProperty` is a thin dispatcher that
    # calls each in turn.
    # These are plain function declarations referenced by the dispatcher
    # at call time, so they MUST embed AFTER web-compat-style-decl.js
    # (and before any consumer that triggers a style apply).
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-layout.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-paint.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-typography.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-transform.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-misc.js
    # _cssToFlex + __cssProperties__ IIFE +
    # setProperty/getPropertyValue/removeProperty. Loaded AFTER
    # web-compat-style-decl.js so the CSSStyleDeclaration constructor
    # + _applyProperty prototype method are in scope when the IIFE
    # walks the property list and installs reflection.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-helpers.js
    # Browser-shaped Element.animate() shim. Loaded after Element,
    # event helpers, and style reflection; resolves rAF lazily at play time.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-animation.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
    # CSS selector engine. Loaded AFTER the parent so document + Element
    # are in scope when underscore-prefixed selector helpers are
    # dispatched from document.querySelector / .querySelectorAll.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document-selectors.js
    # WebGPU mock factories. Loaded AFTER the parent so GPU* usage
    # constants installed by document.js (GPUTextureUsage etc.) are in
    # scope when the factory bodies resolve them at call time.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document-gpu-mock.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-gpu-buffered.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-observers.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-scheduler.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/import-runtime.js
)
set(PULP_JS_EMBEDDED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.hpp)
set(PULP_JS_EMBEDDED_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.cpp)

# embed_js.cmake's `string(SUBSTRING)` chunker operates on
# BYTES, so a 12000-byte chunk boundary that lands inside a UTF-8
# multi-byte codepoint splits the codepoint mid-sequence. The result
# is a generated file with invalid UTF-8 at chunk boundaries; MSVC
# then mis-parses the trailing `)__JS__"` delimiter, walks past it
# into the next R"__JS__()__JS__" header, and reports C2026 ("string
# too big") on the runaway literal. The JS preludes contain UTF-8
# (em-dashes, smart quotes), so chunk boundaries can land on any byte
# boundary.
#
# The Python implementation (embed_js.py) slices on Python str, which
# is char-aware, so chunk boundaries always fall between codepoints.
# Use that implementation for generated prelude sources.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Split the embedded preludes into an extern-linked TU so a
# JS-prelude edit no longer recompiles the large widget_bridge.cpp.
#
# * The DECLARATION header is generated here at *configure* time from the
#   prelude file list, written only-if-changed. Its content depends only on
#   the file *names* (not their contents), so editing a .js prelude never
#   bumps its mtime — and thus never recompiles widget_bridge.cpp, which
#   includes only this header. (Mirrors tools/cmake/PulpEmbedData.cmake.)
# * The DEFINITION .cpp is generated at *build* time by embed_js.py and
#   compiled as its own translation unit; a .js edit rebuilds only this .cpp
#   + relink.
#
# The var-name derivation (stem with '-' -> '_') MUST match embed_js.py.
set(_pulp_prelude_decls
    "#pragma once\n// Auto-generated from the JS prelude file list — do not edit\n\nnamespace pulp::view::preludes {\n\n")
foreach(_prelude ${PULP_JS_PRELUDES})
    get_filename_component(_prelude_stem "${_prelude}" NAME_WE)
    string(REPLACE "-" "_" _prelude_var "${_prelude_stem}")
    string(APPEND _pulp_prelude_decls "extern const char* const ${_prelude_var};\n")
endforeach()
string(APPEND _pulp_prelude_decls "\n}  // namespace pulp::view::preludes\n")

set(_pulp_existing_decls "")
if(EXISTS "${PULP_JS_EMBEDDED_HEADER}")
    file(READ "${PULP_JS_EMBEDDED_HEADER}" _pulp_existing_decls)
endif()
if(NOT "${_pulp_existing_decls}" STREQUAL "${_pulp_prelude_decls}")
    file(WRITE "${PULP_JS_EMBEDDED_HEADER}" "${_pulp_prelude_decls}")
endif()

add_custom_command(
    OUTPUT ${PULP_JS_EMBEDDED_SOURCE}
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.py
        ${PULP_JS_EMBEDDED_SOURCE}
        ${PULP_JS_PRELUDES}
    DEPENDS
        ${PULP_JS_PRELUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.py
    COMMENT "Embedding JS preludes into C++ source (UTF-8-safe Python chunker)"
    VERBATIM
)

# The generated .cpp is a real pulp-view-script source: CMake wires the
# custom-command dependency automatically, so no separate aggregator target
# is needed. widget_bridge.cpp includes only the (stable) declaration header.
target_sources(pulp-view-script PRIVATE ${PULP_JS_EMBEDDED_SOURCE})
