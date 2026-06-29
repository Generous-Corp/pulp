# PULP_ENABLE_JS — when OFF, pulp::view is NATIVE-ONLY: the JS scripting +
# web-compat layer (pulp::view-script — QuickJS/JSC/V8 engine + the widget
# bridge + the embedded JS preludes) is neither built nor linked, so a standalone
# built on pulp::view contains zero JS-engine symbols and ships a smaller binary.
# The faithful-vector design-import lane (native Skia draw_svg + native overlays)
# and every native widget still work — view-core is self-contained and carries no
# JS dependency (view-script depends on view-core, not the reverse). Default ON to
# preserve the JS-scripted GPU UI path.
option(PULP_ENABLE_JS "Build the JS scripting + web-compat layer (pulp::view-script)" ON)

add_library(pulp-view-core)
add_library(pulp::view-core ALIAS pulp-view-core)
# Consumers (and view-core's own headers) gate any optional JS use on this.
target_compile_definitions(pulp-view-core PUBLIC PULP_ENABLE_JS=$<BOOL:${PULP_ENABLE_JS}>)

add_library(pulp-view-script)
add_library(pulp::view-script ALIAS pulp-view-script)

add_library(pulp-view src/placeholder.cpp)
add_library(pulp::view ALIAS pulp-view)
if(PULP_ENABLE_JS)
    target_link_libraries(pulp-view PUBLIC pulp::view-script)
else()
    # Native-only: link the JS-free core directly and keep view-script defined
    # (so its still-evaluated config blocks below never dangle) but out of the
    # default build and every binary, so no JS engine is compiled or shipped.
    target_link_libraries(pulp-view PUBLIC pulp::view-core)
    set_target_properties(pulp-view-script PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # Authoritative native-only guard. The binary nm-check (AssertNoJsSymbols.cmake)
    # cannot catch a view-script leak: view-script is a static archive, so unless
    # a symbol is referenced the linker drops it from the final binary even when
    # the archive IS linked — the symbol-grep passes on a misconfigured build.
    # The link INTERFACE is the reliable signal, available here at configure time.
    get_target_property(_pv_links pulp-view LINK_LIBRARIES)
    if(_pv_links MATCHES "view-script")
        message(FATAL_ERROR
            "PULP_ENABLE_JS=OFF but pulp-view links pulp-view-script "
            "(LINK_LIBRARIES=${_pv_links}). The native-only build must not pull "
            "the JS scripting layer. Remove the view-script link from the "
            "PULP_ENABLE_JS=OFF path in core/view/CMakeLists.txt.")
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

# ── Embed JS preludes as C++ string constants ────────────────────────────────
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

foreach(_pulp_view_target pulp-view-core pulp-view-script)
    target_include_directories(${_pulp_view_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_view_target)

target_include_directories(pulp-view-script
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}  # For generated web_compat_preludes_gen.{hpp,cpp}
)

# ── JS engine backend selection ──────────────────────────────────────────────
# PULP_JS_ENGINE: auto | quickjs | jsc | v8
#   auto    → JSC on Apple, QuickJS elsewhere (never V8 — V8 is opt-in)
#   quickjs → QuickJS only (always available, portable)
#   jsc     → JavaScriptCore (Apple platforms only)
#   v8      → V8 via the pinned sealed libv8 (FindV8.cmake →
#             external/v8-build/<platform>/, populated by
#             tools/scripts/fetch_v8_for_release.py). Not supported on iOS.
set(PULP_JS_ENGINE "auto" CACHE STRING "JS engine backend (auto|quickjs|jsc|v8)")
set_property(CACHE PULP_JS_ENGINE PROPERTY STRINGS auto quickjs jsc v8)
# Advanced local-experiment overrides only — normal v8 builds need none (FindV8
# resolves external/v8-build or a baked $V8_DIR). When v8 is selected, the
# provider is the pinned sealed libv8, nothing else.
set(V8_LIBRARY_PATH "" CACHE FILEPATH "Advanced: full path to a V8 runtime library (local experiments)")

# ── V8 provider-identity reporting (additive) ────────────────────────────────
# The pinned V8 provider is resolved by FindV8.cmake (external/v8-build/<platform>/,
# fetched by tools/scripts/fetch_v8_for_release.py). The V8 backend block below
# fills in the provider-identity vars from the resolved sealed artifact and emits
# them as compile definitions. Defaults describe an unknown/unselected provider.
option(PULP_VALIDATE_V8_PROVIDER_STRICT
    "Run the strict (no skip-pass) V8 provider-identity + cube-coexistence CTest" OFF)
set(PULP_V8_PROVIDER_KIND "")
set(PULP_V8_PROVIDER_PATH "")
set(PULP_V8_EXPECTED_RUNTIME_VERSION "")

# QuickJS backend is always compiled (portable fallback)
set(PULP_JS_ENGINE_SOURCES
    src/js_quickjs_engine.cpp
    src/js_engine_factory.cpp
)

set(_pulp_ubsan_enabled OFF)
if(PULP_SANITIZER STREQUAL "undefined"
        OR CMAKE_CXX_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined"
        OR CMAKE_C_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined")
    set(_pulp_ubsan_enabled ON)
endif()

if(_pulp_ubsan_enabled AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    # CHOC's header-only QuickJS backend is tracked under #1987 for UBSan-only
    # exception corruption. Keep UBSan on Pulp's script/runtime code while
    # compiling only that vendored backend without undefined instrumentation.
    set_source_files_properties(src/js_quickjs_engine.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-sanitize=undefined")
endif()
unset(_pulp_ubsan_enabled)

# V8 backend — provider is the pinned sealed libv8 (FindV8.cmake →
# external/v8-build/<platform>/, fetched by tools/scripts/fetch_v8_for_release.py).
# js_v8_engine.cpp is ALWAYS compiled; it is #ifdef PULP_HAS_V8 internally and
# becomes a stub when V8 is not linked. V8 is linked ONLY when explicitly selected
# with PULP_JS_ENGINE=v8 — `auto` never silently pulls in V8.
list(APPEND PULP_JS_ENGINE_SOURCES src/js_v8_engine.cpp)
if(PULP_JS_ENGINE STREQUAL "v8")
    # iOS cannot JIT — V8 is forbidden in iOS apps / AUv3 extensions. Hard error,
    # not a silent fallback, so a misconfigured iOS build fails loudly.
    if(IOS OR PULP_IOS)
        message(FATAL_ERROR
            "PULP_JS_ENGINE=v8 is not supported on iOS: V8 requires JIT, which iOS "
            "apps and AUv3 extensions forbid. iOS uses JSC (the default on Apple).")
    endif()
    include(${CMAKE_SOURCE_DIR}/tools/cmake/FindV8.cmake)
    if(NOT PULP_V8_FOUND)
        message(FATAL_ERROR
            "PULP_JS_ENGINE=v8 selected but no sealed V8 was found. Populate it with:\n"
            "    python3 tools/scripts/fetch_v8_for_release.py <platform>\n"
            "(unpacks to external/v8-build/), or point V8_DIR at a baked V8, or set "
            "the advanced V8_INCLUDE_DIR + V8_LIB_DIR overrides for a local build.")
    endif()
    target_link_libraries(pulp-view-script PRIVATE v8::v8)
    set(PULP_HAS_V8_ACTUAL ON)

    # Provider identity — the pinned provider IS the sealed v8-builder libv8
    # (FindV8 resolves external/v8-build/<platform>/). Surface its kind, path,
    # and the manifest-pinned runtime version so the runtime can report and
    # cross-check the provider. The js_v8_engine.cpp accessors that read these
    # defines are additive and degrade gracefully when a define is empty.
    set(PULP_V8_PROVIDER_KIND "v8builder")
    set(PULP_V8_PROVIDER_PATH "${V8_RUNTIME_LIBRARY}")
    set(PULP_V8_EXPECTED_RUNTIME_VERSION "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/deps/manifest.json")
        file(READ "${CMAKE_SOURCE_DIR}/tools/deps/manifest.json" _pulp_v8_manifest_text)
        # Match the V8 dependency's pinned tag (e.g. "v8-15.1.27") and reduce it
        # to the bare runtime version (15.1.27) the runtime reports at startup.
        if(_pulp_v8_manifest_text MATCHES "\"name\"[ \t]*:[ \t]*\"V8\"[^}]*\"version\"[ \t]*:[ \t]*\"v8-([0-9][^\"]*)\"")
            set(PULP_V8_EXPECTED_RUNTIME_VERSION "${CMAKE_MATCH_1}")
        endif()
        unset(_pulp_v8_manifest_text)
    endif()

    # Publish the identity to the global cache so sibling subdirectories added
    # after core/view (examples/threejs-native-demo's strict provider-identity
    # CTest) can read the resolved kind + pinned version without re-deriving it.
    set(PULP_V8_PROVIDER_KIND "${PULP_V8_PROVIDER_KIND}" CACHE INTERNAL "Resolved V8 provider kind")
    set(PULP_V8_PROVIDER_PATH "${PULP_V8_PROVIDER_PATH}" CACHE INTERNAL "Resolved V8 provider library path")
    set(PULP_V8_EXPECTED_RUNTIME_VERSION "${PULP_V8_EXPECTED_RUNTIME_VERSION}" CACHE INTERNAL "Manifest-pinned V8 runtime version")

    # Windows: the sealed v8.dll exposes the Chromium libc++ (__Cr) ABI with
    # pointer compression ON, so the V8 translation unit must be compiled
    # with clang-cl + that libc++ + V8_COMPRESS_POINTERS. This is a no-op off
    # Windows; on Windows it hard-fails if the compiler is MSVC cl rather
    # than clang-cl. See tools/cmake/PulpV8Windows.cmake.
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpV8Windows.cmake)
    pulp_v8_windows_apply_abi(pulp-view-script)
endif()

# JSC backend (Apple platforms only)
if(APPLE)
    list(APPEND PULP_JS_ENGINE_SOURCES src/js_jsc_engine.mm)
endif()

# NSBundle-based loader for the embedded Three.js
# IIFE bundle. Only compiled on iOS / iOS Simulator; desktop/macOS
# lanes use src/threejs_resources_stub.cpp (always returns nullopt).
if(APPLE AND PULP_IOS)
    target_sources(pulp-view-core PRIVATE src/threejs_resources_apple.mm)
endif()

# Set compile definitions for engine availability / defaults
if(PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view-script PRIVATE PULP_HAS_V8=1)

    # Provider identity, consumed by V8Engine::provider_kind()/provider_path()/
    # expected_runtime_version(). These describe the sealed v8-builder artifact
    # independently of the runtime version V8 reports at startup.
    target_compile_definitions(pulp-view-script PRIVATE
        PULP_V8_PROVIDER_KIND="${PULP_V8_PROVIDER_KIND}"
        PULP_V8_PROVIDER_PATH="${PULP_V8_PROVIDER_PATH}"
        PULP_V8_EXPECTED_RUNTIME_VERSION="${PULP_V8_EXPECTED_RUNTIME_VERSION}")
endif()

# Only set non-QuickJS default when explicitly requested (not auto)
if(PULP_JS_ENGINE STREQUAL "jsc")
    if(APPLE)
        target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_JSC=1)
    endif()
elseif(PULP_JS_ENGINE STREQUAL "v8" AND PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_V8=1)
endif()
# auto → QuickJS default (backward compatible with all existing code)

# Per-platform TextAccessibilityNode backend selection.
# Each platform leg overrides the cross-platform "none" default at link
# time by compiling a platform-specific TU INSTEAD of the default
# src/text_accessibility.cpp. The platform TUs export the same symbols
# (register_text_accessibility_node etc.) so callers don't need to
# branch on the build target.
#
# macOS: NSAccessibility-backed (.mm, "macos-ax").
# Windows: UIA IRawElementProviderSimple ("windows-uia").
# Linux: AccessKit stub ("linux-accesskit-stub").
# Other platforms: default C++ "none" backend.
if(APPLE)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/mac/text_accessibility_macos.mm)
elseif(WIN32)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/win/text_accessibility_windows.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/linux/text_accessibility_linux.cpp)
else()
    set(PULP_TEXT_ACCESSIBILITY_SOURCE src/text_accessibility.cpp)
endif()

target_sources(pulp-view-core PRIVATE
    src/accessibility.cpp
    src/import_validation_bridge.cpp
    src/lottie_view.cpp
    ${PULP_TEXT_ACCESSIBILITY_SOURCE}
    src/threejs_resources_stub.cpp
    src/table_model.cpp
    src/modulation_matrix.cpp
    src/modulation_matrix_widget.cpp
    src/accessibility_tree.cpp
    src/image_cache.cpp
    src/resizable_shell.cpp
    src/theme.cpp
    src/theme_contrast.cpp
    src/theme_presets.cpp
    src/appearance_tracker.cpp
    src/motion_preferences.cpp
    src/asset_manager.cpp
    src/script_event_dispatch.cpp
    src/view.cpp
    src/yoga_layout.cpp
    src/layout_snapshot.cpp
    src/widgets.cpp
    src/widgets/label.cpp
    src/widgets/visualizers.cpp
    src/editor_bridge.cpp
    src/inspector.cpp
    src/sdl_window_host.cpp
    src/auto_ui.cpp
    src/text_edit_model.cpp
    src/text_editor.cpp
    src/text_editor_clipboard.cpp
    src/text_editor_ime.cpp
    src/text_editor_paint.cpp
    src/ui_components.cpp
    src/context_menu.cpp
    src/scroll_bar.cpp
    src/relative_point.cpp
    src/css_gradient.cpp
    src/w3c_tokens.cpp
    src/widget_skin_derive.cpp
    src/screenshot_compare.cpp
    src/screenshot_gpu.cpp
    src/app_framework.cpp
    src/canvas_widget.cpp
    src/design_frame_view.cpp
    src/svg_path_widget.cpp
    src/widgets/svg_rect.cpp
    src/widgets/svg_line.cpp
    src/visualization_bridge.cpp
    src/waveform_editor_primitives.cpp
    src/waveform_gpu_primitives.cpp
    src/waveform_gpu_render_controller.cpp
    src/waveform_headless_render_backend.cpp
    src/frame_clock.cpp
    src/motion.cpp
    src/motion_geometry.cpp
    src/motion_cost.cpp
    src/window_manager.cpp
    src/eq_curve_view.cpp
    src/midi_keyboard.cpp
    src/musical_typing_keyboard.cpp
    src/musical_typing_keyboard_svg.cpp
    src/channel_strip_view.cpp
    src/channel_strip_view_svg.cpp
    src/range_slider_view.cpp
    src/range_slider_view_svg.cpp
    src/inline_value_editor_view.cpp
    src/inline_value_editor_view_svg.cpp
    src/property_panel_view.cpp
    src/property_panel_view_svg.cpp
    src/group_box_view.cpp
    src/group_box_view_svg.cpp
    src/number_box_states_view.cpp
    src/number_box_states_view_svg.cpp
    src/knob_modulation_view.cpp
    src/knob_modulation_view_svg.cpp
    src/waveform_recorder_view.cpp
    src/waveform_recorder_view_svg.cpp
    src/color_picker.cpp
    src/theme_editor.cpp
    src/file_drop_zone.cpp
    src/sample_asset_drop_target.cpp
    src/drag_drop.cpp
    src/split_view.cpp
    src/property_list.cpp
    src/breadcrumb.cpp
    src/buttons.cpp
    src/gap_widgets.cpp
    src/widget_gallery.cpp
    src/design_system.cpp
    src/sampler_starter.cpp
    src/file_browser.cpp
    src/file_chooser.cpp
    src/code_editor.cpp
    src/code_editor_tokenizer.cpp
    src/table.cpp
    src/toolbar.cpp
    src/concertina_panel.cpp
    src/splash_screen.cpp
    src/live_constant_editor.cpp
    src/inspector_window.cpp
    src/audio_inspector_panel.cpp
    src/audio_inspector_window.cpp
    src/command_registry.cpp
    src/key_mapping_editor.cpp
    src/window_classes.cpp
    src/bubble_message.cpp
)

target_sources(pulp-view-script PRIVATE
    ${PULP_JS_ENGINE_SOURCES}
    src/script_engine.cpp
    src/js_engine_recommend.cpp
    src/widget_bridge.cpp
    src/widget_bridge/accessibility_api.cpp
    src/widget_bridge/animation_api.cpp
    src/widget_bridge/dom_api.cpp
    src/widget_bridge/event_api.cpp
    src/widget_bridge/gpu_api.cpp
    src/widget_bridge/gpu_common.cpp
    src/widget_bridge/style_cursor_api.cpp
    src/widget_bridge/style_effects_api.cpp
    src/widget_bridge/style_rn_compat_api.cpp
    src/widget_bridge/style_state_api.cpp
    src/widget_bridge/style_storage_api.cpp
    src/widget_bridge/style_visual_api.cpp
    src/widget_bridge/style_visibility_api.cpp
    src/widget_bridge/layout_api.cpp
    src/widget_bridge/list_style_api.cpp
    src/widget_bridge/metadata_api.cpp
    src/widget_bridge/platform_services_api.cpp
    src/widget_bridge/runtime_api.cpp
    src/widget_bridge/state_binding_api.cpp
    src/widget_bridge/storage_assets_api.cpp
    src/widget_bridge/shader_api.cpp
    src/widget_bridge/svg_api.cpp
    src/widget_bridge/theme_api.cpp
    src/widget_bridge/tokens_api.cpp
    src/widget_bridge/widget_assets_api.cpp
    src/widget_bridge/widget_schema_api.cpp
    src/widget_bridge/factory_api.cpp
    src/widget_bridge/widget_value_controls_api.cpp
    src/widget_bridge/widget_value_basic_api.cpp
    src/widget_bridge/typography_api.cpp
    src/widget_bridge/text_runs_api.cpp
    src/widget_bridge/border_box_api.cpp
    src/widget_bridge/outline_api.cpp
    src/widget_bridge/border_radius_api.cpp
    src/widget_bridge/border_side_api.cpp
    src/widget_bridge/canvas2d_api.cpp
    src/widget_bridge/widget_value_list_api.cpp
    src/widget_bridge/widget_value_content_api.cpp
    src/widget_bridge_input.cpp
    src/hot_reload.cpp
    src/scripted_ui.cpp
)

# ── Design-import authoring cluster (PULP_ENABLE_DESIGN_IMPORT) ───────────────
# Importers, codegen, lock-to-source, JSX-lock, and the runtime design-import
# opt-in. Gated out of shipped plugins; the runtime W3C token pair
# (src/w3c_tokens.cpp) and the runtime widgets (canvas/svg/design_frame_view/
# css_gradient/visualization/app_framework/widget_skin_derive) stay
# always-compiled above.
if(PULP_ENABLE_DESIGN_IMPORT)
    target_sources(pulp-view-core PRIVATE
        src/anchor_strategy.cpp
        src/design_export.cpp
        src/design_import.cpp
        src/design_binding_metadata.cpp
        src/design_ir_json.cpp
        src/recognition_resolver.cpp
        src/design_ir_analysis.cpp
        src/design_import_v0_tsx.cpp
        src/design_import_native_common.cpp
        src/design_cpp_codegen.cpp
        src/design_swift_codegen.cpp
        src/design_tokens.cpp
        src/design_codegen.cpp
        src/design_fidelity.cpp
        src/lock_to_source.cpp
        src/jsx_lock.cpp
        src/design_import_shortcuts.cpp
        src/design_import_designmd.cpp
        src/token_lock.cpp
    )
    target_sources(pulp-view-script PRIVATE
        src/widget_bridge/runtime_import_api.cpp
        src/claude_bundle.cpp
        src/claude_bundle_sources.cpp
    )
    target_compile_definitions(pulp-view-core PUBLIC PULP_HAS_DESIGN_IMPORT=1)
    target_compile_definitions(pulp-view-script PUBLIC PULP_HAS_DESIGN_IMPORT=1)
else()
    # No-op WidgetBridge::install_runtime_import_handlers() so callers still link.
    target_sources(pulp-view-script PRIVATE
        src/widget_bridge/runtime_import_api_stub.cpp
    )
    target_compile_definitions(pulp-view-core PUBLIC PULP_HAS_DESIGN_IMPORT=0)
    target_compile_definitions(pulp-view-script PUBLIC PULP_HAS_DESIGN_IMPORT=0)
endif()

# WebView support — compiled separately to avoid platform WebView runtime
# auto-loading in plugin hosts. Only link into standalone apps, not
# AU/VST3/CLAP plugins.
# The ObjC runtime loads WebKit on dylib load if any WKWebView references exist,
# which crashes Logic Pro's sandboxed AUHostingServiceXPC process.
# ── Linux AT-SPI accessibility ─────────────────────────────────────────────
#
# Pulp publishes accessibility info to Linux screen readers (Orca) by
# implementing the AT-SPI2 *wire protocol* directly over D-Bus
# (pulp::platform::DBus, libdbus-1 dlopen'd, honest-fail). There is NO
# build-time or link-time dependency on libatk / libatspi / atk-bridge-2.0 —
# the protocol numbers live in core/view/include/pulp/view/platform/atspi_mapping.hpp
# and accessibility_linux.cpp exports the objects. Roles/states are unit-tested
# offline; the live AT stack is exercised in the Linux VM lane. This also keeps
# the build MIT-clean (the ATK runtime is LGPL-adjacent). PkgConfig is still
# discovered below for X11 / WebView.
find_package(PkgConfig QUIET)

option(PULP_BUILD_WEBVIEW "Include native WebView support (WebKit / WebView2 / WebKitGTK)" OFF)
if(PULP_BUILD_WEBVIEW)
    target_sources(pulp-view-core PRIVATE src/web_view.cpp)
    # PUBLIC so tests and downstream callers see the same value the
    # detector TU was compiled with.
    target_compile_definitions(pulp-view-core PUBLIC PULP_BUILD_WEBVIEW=1)
    if(APPLE AND NOT PULP_IOS)
        # In-process WKWebView snapshot → PNG so native WebView overlays are
        # headlessly capturable (WebViewPanel::snapshot_png()).
        target_sources(pulp-view-core PRIVATE platform/mac/web_view_snapshot_mac.mm)
        target_link_libraries(pulp-view-core PRIVATE "-framework WebKit")
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
        target_link_libraries(pulp-view-core PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT2GTK)
    # Android: WebView is handled by the Kotlin layer, not native GTK
    endif()
else()
    # Backend detector honors the documented "none if disabled" contract
    # When WebView is excluded from the build,
    # detect_webview_backend() must return "none" so callers can
    # distinguish "build excluded WebView" from "OS has no WebView".
    target_compile_definitions(pulp-view-core PUBLIC PULP_BUILD_WEBVIEW=0)
endif()

# Backend detection TUs — always compiled into pulp-view-core regardless
# of PULP_BUILD_WEBVIEW so callers can distinguish "this build excluded
# WebView" from "this OS has no usable WebView backend installed".
# `web_view_backend.cpp` owns the symbol on Apple / Android / generic
# OSes; the Windows + Linux per-platform TUs own it on their OS so the
# runtime probe (LoadLibrary / dlopen) can run before the answer is
# returned.
target_sources(pulp-view-core PRIVATE src/web_view_backend.cpp)
if(WIN32)
    target_sources(pulp-view-core PRIVATE platform/win/webview2_backend.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    target_sources(pulp-view-core PRIVATE platform/linux/webkitgtk_backend.cpp)
endif()

# CHOC headers (JS engine, utilities)
foreach(_pulp_view_target pulp-view-core pulp-view-script)
    target_include_directories(${_pulp_view_target} PUBLIC
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_view_target)

target_link_libraries(pulp-view-core PUBLIC pulp::canvas pulp::events pulp::state pulp::signal pulp::platform pulp::audio)
# pulp::host is iOS-restricted: iOS sandbox disallows dlopen of arbitrary
# third-party plugins (no VST3 / CLAP / LV2 hosting), and pulling host into
# the link line on iOS surfaces a `long double to_chars` libc++ availability
# error via `scanner_clap.cpp` → `runtime::log_warn` → `std::format`. The
# header-only `plugin_manager_panel.hpp` lives in pulp-view and includes
# `pulp/host/scanner.hpp`, so consumers that instantiate it on desktop still
# need the symbol — link there. iOS AUv3 .appex bundles never instantiate
# the panel, so dropping the PUBLIC dep there is safe and unblocks the
# iOS AUv3 build smoke.
if(NOT IOS)
    target_link_libraries(pulp-view-core PUBLIC pulp::host)
    # graph_editor_view.cpp uses host::SignalGraph / host::Connection / host::GraphNode
    # directly, so its translation unit only compiles when pulp::host is linked.
    # iOS skips the source entirely; the header is also iOS-guarded so any
    # downstream consumer that includes <pulp/view/widgets/graph_editor_view.hpp>
    # on iOS gets a clean compile-time skip rather than a "file not found" on
    # <pulp/host/signal_graph.hpp>.
    target_sources(pulp-view-core PRIVATE src/graph_editor_view.cpp)
endif()
target_link_libraries(pulp-view-script PUBLIC pulp::view-core)
# design_import.cpp uses pulp::runtime::base64_decode +
# gzip_decompress to unpack Claude Design's base64-gzip envelope.
# PUBLIC because the public header model_manager_view.hpp includes
# <pulp/runtime/model_store.hpp> (exposes runtime::ModelListResult in its API),
# so downstream targets that link only pulp::view must inherit the runtime
# include path + link.
target_link_libraries(pulp-view-core PUBLIC pulp::runtime)
target_link_libraries(pulp-view-script PRIVATE pulp::runtime)

# yaml-cpp (MIT) — DESIGN.md frontmatter parsing.
# Wrapped in $<BUILD_INTERFACE:...> because yaml-cpp is a build-only
# dependency: Pulp's public headers never expose YAML types, so
# downstream consumers of installed pulp-view (via PulpTargets export
# set) must not see yaml-cpp as a transitive requirement.
target_link_libraries(pulp-view-core PRIVATE $<BUILD_INTERFACE:yaml-cpp::yaml-cpp>)

# JavaScriptCore framework for JSC backend (Apple only)
if(APPLE)
    target_link_libraries(pulp-view-script PRIVATE "-framework JavaScriptCore")
endif()

if(WIN32)
    # Process spawning routes through pulp::platform::ChildProcess.
endif()

# Yoga layout engine (MIT)
if(PULP_HAS_YOGA)
    target_link_libraries(pulp-view-core PRIVATE
        $<BUILD_INTERFACE:yogacore>
        $<INSTALL_INTERFACE:Pulp::yogacore>
    )
    target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_YOGA=1)
endif()

# SDL3 for cross-platform windowing
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-view-core PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
    )
    target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_SDL3=1)
endif()

# Platform-specific window host and frameworks
if(APPLE)
    if(PULP_IOS)
        target_sources(pulp-view-core PRIVATE
            platform/ios/window_host_ios.mm
            platform/ios/plugin_view_host_ios.mm
            platform/ios/accessibility_ios.mm
            # Bridge stubs compile on iOS too. They provide set_factory /
            # set_screenshot_provider / has_* registration APIs
            # unconditionally; create/render impls stay gated by
            # their own #if checks so the iOS-native window +
            # plugin-view impls aren't shadowed. screenshot has no
            # iOS-native impl yet, so the stub's provider-backed
            # path is the one iOS actually uses.
            src/plugin_view_host_stub.cpp
            src/screenshot_stub.cpp
            src/window_host_stub.cpp
        )
        target_link_libraries(pulp-view-core PRIVATE
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework QuartzCore"
        )
        # Wire iOS GPU window host (window_host_ios.mm's
        # IOSGpuWindowHost branch). Mirrors the mac branch below:
        # `pulp::render` exposes Dawn / Skia Graphite + the GpuSurface
        # interface, and the Metal framework backs the CAMetalLayer.
        # `pulp::render` is gated by PULP_HAS_SKIA so a no-Skia iOS
        # configure (CG-only) still links cleanly.
        if(PULP_HAS_SKIA)
            target_link_libraries(pulp-view-core PRIVATE pulp::render
                "-framework Metal"
            )
        endif()
    else()
        target_sources(pulp-view-core PRIVATE
            platform/mac/window_host_mac.mm
            platform/mac/window_host_mac_capture.mm
            platform/mac/window_host_mac_geometry.mm
            platform/mac/window_host_mac_text_input.mm
            platform/mac/window_manager_mac.mm
            platform/mac/plugin_view_host_mac.mm
            platform/mac/screenshot_mac.mm
            platform/mac/drag_drop_mac.mm
            platform/mac/accessibility_mac.mm
            platform/mac/appearance_mac.mm
            platform/mac/motion_preferences_mac.mm
            # Host-backend registration APIs. The stub files compile
            # set_factory/set_screenshot_provider etc.
            # unconditionally; only the create/render impls are
            # gated by `#if !defined(__APPLE__)`. Including them
            # here surfaces the registration API on mac without
            # conflicting with the native impls.
            src/plugin_view_host_stub.cpp
            src/screenshot_stub.cpp
            src/window_host_stub.cpp
        )
        target_link_libraries(pulp-view-core PRIVATE
            "-framework CoreServices"
            "-framework Cocoa"
            "-framework ImageIO"
            "-framework UniformTypeIdentifiers"
            # CVDisplayLink drives the continuous-frame / idle pump in BOTH the
            # CPU (CoreGraphics) and GPU plugin hosts, so CoreVideo must link
            # even when Skia is absent (the CG-only fallback build).
            "-framework CoreVideo"
        )
        # GPU rendering: link render module and Metal for CAMetalLayer-backed window host
        if(PULP_HAS_SKIA)
            target_link_libraries(pulp-view-core PRIVATE pulp::render
                "-framework Metal"
                "-framework QuartzCore"
            )
        endif()
    endif()
elseif(WIN32)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/win/accessibility_win.cpp
        platform/win/appearance_win.cpp
        platform/win/motion_preferences_win.cpp
        # Standalone outbound file-drag backend (free begin_file_drag via OLE).
        # Unconditional — DoDragDrop has no Skia/GPU dependency, so a non-GPU
        # standalone build still drags files out. ole32 is linked below.
        platform/win/drag_drop_win.cpp
    )
    # Win/Linux parity. When Skia is available, add the built-in Skia raster
    # screenshot backend and the native HWND PluginViewHost.
    # plugin_view_host_stub.cpp stays (it owns the always-compiled
    # set_factory/has_factory registration API + the factory-dispatching
    # create()); the native impl registers ITSELF as the factory via
    # register_platform_plugin_view_host(). screenshot_skia.cpp self-guards
    # on PULP_HAS_SKIA and supersedes the stub's render_* fallback (which is
    # now compiled only when !PULP_HAS_SKIA).
    if(PULP_HAS_SKIA)
        target_sources(pulp-view-core PRIVATE
            src/screenshot_skia.cpp
            platform/win/plugin_view_host_win.cpp
        )
        # The native HWND host supplies register_platform_plugin_view_host();
        # tell the stub not to compile its no-op default (one definition).
        set_source_files_properties(src/plugin_view_host_stub.cpp PROPERTIES
            COMPILE_DEFINITIONS PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST=1)
    endif()
    # UIAutomationCore for UiaClientsAreListening plus
    # UiaReturnRawElementProvider + UiaRaiseStructureChangedEvent.
    target_link_libraries(pulp-view-core PRIVATE uiautomationcore)
    # OLE drag-drop on the plugin-host child HWND (WinPluginViewHost):
    # ole32 = OleInitialize/RegisterDragDrop/ReleaseStgMedium; shell32 =
    # DragQueryFileW for CF_HDROP file lists.
    target_link_libraries(pulp-view-core PRIVATE ole32 shell32)
elseif(ANDROID)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
    )
    # Android accessibility wired via Kotlin TalkBack integration
elseif(UNIX)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/linux/accessibility_linux.cpp
    )
    # Win/Linux parity. Same split as the WIN32 branch: when Skia is available
    # add the built-in Skia raster screenshot backend and the native X11
    # PluginViewHost (which registers itself via
    # register_platform_plugin_view_host()). The X11 host links Xlib; missing
    # X11 dev headers degrade to the headless-render-only path.
    if(PULP_HAS_SKIA)
        target_sources(pulp-view-core PRIVATE src/screenshot_skia.cpp)
        find_package(X11)
        if(X11_FOUND)
            target_sources(pulp-view-core PRIVATE
                platform/linux/plugin_view_host_linux.cpp
                # Standalone outbound file-drag backend (free begin_file_drag via
                # XDND source). Gated with PULP_HAS_X11 so the drag_drop.cpp no-op
                # drops out only when this real backend is compiled.
                platform/linux/drag_drop_linux.cpp)
            target_link_libraries(pulp-view-core PRIVATE ${X11_LIBRARIES})
            target_include_directories(pulp-view-core PRIVATE ${X11_INCLUDE_DIR})
            target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_X11=1)
            # The native X11 host supplies register_platform_plugin_view_host();
            # tell the stub not to compile its no-op default (one definition).
            set_source_files_properties(src/plugin_view_host_stub.cpp PROPERTIES
                COMPILE_DEFINITIONS PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST=1)
        else()
            message(STATUS "Pulp: X11 not found — Linux PluginViewHost limited to headless render")
        endif()
    endif()
    # Linux AT-SPI needs no link deps: accessibility_linux.cpp
    # speaks the AT-SPI2 wire protocol directly over pulp::platform::DBus
    # (libdbus-1 dlopen'd). No atk-bridge-2.0 / libatspi at build time.
endif()

# Link against render for native GPU bridge (Three.js, WebGPU canvas).
# visualization_bridge.hpp forward-declares PerfCounters and the dereferencing
# code lives in visualization_bridge.cpp, so the link stays PRIVATE regardless
# of PULP_BENCHMARK. Downstream targets that only use view APIs do NOT need to
# link pulp::render when PULP_BENCHMARK is enabled.
if(TARGET pulp-render)
    target_link_libraries(pulp-view-core PRIVATE pulp::render)
    target_link_libraries(pulp-view-script PRIVATE pulp::render)
    # Enables the offscreen-GPU view-capture path (render_to_png_gpu / capture_view
    # via pulp::render::HeadlessSurface) in src/screenshot_gpu.cpp. Gate on Skia,
    # not merely on the pulp-render target: in a no-Skia build pulp-render still
    # exists but HeadlessSurface::create() is compiled as the stub that always
    # returns null, so the GPU path could never produce bytes. Defining the macro
    # there would make has_gpu_capture() report true and suppress the honest raster
    # fallback, ending in a no-bytes failure. Only claim GPU capture when Skia (and
    # thus a real HeadlessSurface) is present; otherwise capture degrades to raster
    # honestly (has_gpu_capture()==false).
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-view-core PRIVATE PULP_VIEW_HAS_GPU_CAPTURE=1)
    endif()
    # When pulp-render is linked, WindowHost::mark_dirty() can
    # route dirty marks through a vblank-paced RenderLoop. GPU-off builds
    # (sanitizer matrix) have no pulp-render target, so mark_dirty() there
    # degrades to a direct repaint(); the define gates that cleanly.
    target_compile_definitions(pulp-view-core PRIVATE PULP_VIEW_HAS_RENDER_LOOP=1)
endif()
