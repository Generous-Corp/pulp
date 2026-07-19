# PulpWebUi.cmake — reusable browser-UI (Emscripten) build helper.
#
# Compiles Pulp's OWN render stack — core/canvas + core/view + the Ganesh subset
# of core/render — into a DSP-FREE WebAssembly module that paints a real View
# tree into a browser canvas. Requires the Emscripten toolchain (configure with
# `emcmake cmake ...`); under any other toolchain this file is a no-op.
#
# WHY A CURATED SOURCE LIST INSTEAD OF LINKING pulp::view:
# Same reason PulpWam.cmake curates a DSP subset, mirrored across the render
# boundary. `pulp-view` publicly links the JS scripting layer, the SDL window
# host, the design-import authoring cluster, and (through pulp-render) the Dawn
# GPU surface + the WGSL compute kernels — none of which builds for, or belongs
# in, a browser UI module. So this helper compiles the portable UI subset
# directly. It touches no core/*/CMakeLists.txt.
#
# BACKEND: the published wasm Skia slice is Ganesh/WebGL2 (no Dawn, no Graphite),
# so FindSkia's EMSCRIPTEN arm defines SK_GANESH + SK_GL and this list compiles
# skia_surface_ganesh.cpp INSTEAD of skia_surface.cpp / gpu_surface_dawn.cpp.
# Nothing ABOVE the render boundary knows: core/canvas, the widgets, the editor,
# and the plugin UI code are backend-agnostic and a later Graphite-on-wasm
# convergence would not touch any of them. It would, however, touch four files at
# and below the boundary — so "swap one TU" is the shape of the change, not a
# literal file count:
#   1. tools/cmake/FindSkia.cmake — the EMSCRIPTEN backend defines
#      (SK_GANESH;SK_GL;SK_TRIVIAL_ABI -> SK_GRAPHITE;SK_DAWN)
#   2. _PULP_WEBUI_RENDER_SOURCES below — the surface TU
#   3. SkiaSurface::create_webgl / WebGlConfig in core/render/include (the
#      factory is named for the backend it wraps)
#   4. its one call site in core/view/platform/web/window_host_web.cpp
#
# NOT INCLUDED, deliberately:
#   gpu_surface_dawn.cpp / gpu_compute.cpp  — Dawn; the wasm slice has none.
#   skp_capture.cpp / sdl3_surface.cpp / metal_surface_*.mm / render_loop_apple.mm
#   core/view JS scripting (pulp-view-script), design-import authoring,
#   web_view / native_view_host / plugin hosting, and everything under
#   core/view/platform/{mac,linux,win,ios,android}.
#
# headless_surface.cpp IS compiled: the browser host uses its static
# HeadlessSurface::encode_png(). Its offscreen-GPU path (GpuSurface::create_dawn)
# is never called and wasm-ld drops it, so no Dawn reference survives the link.

include_guard(GLOBAL)

if(NOT EMSCRIPTEN)
    message(STATUS "PulpWebUi: web-UI targets skipped (not an Emscripten build).")
    return()
endif()

# Repo root = two levels up from tools/cmake/.
get_filename_component(_PULP_WEBUI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT PULP_ROOT_DIR)
    set(PULP_ROOT_DIR "${_PULP_WEBUI_ROOT}")
endif()

# ── Skia (Ganesh/WebGL2 slice) ────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${_PULP_WEBUI_ROOT}/tools/cmake")
find_package(Skia REQUIRED)
if(NOT SKIA_FOUND)
    message(FATAL_ERROR
        "PulpWebUi: Skia not found. Pass -DSKIA_DIR=<wasm slice root> (the "
        "directory containing build/wasm-gpu/lib/Release/libskia.a).")
endif()

# ── choc (header-only; theme JSON) ────────────────────────────────────────────
if(NOT PULP_WEBUI_CHOC_INCLUDE)
    foreach(_cand
        "${PULP_WAM_CHOC_INCLUDE}"
        "$ENV{PULP_WAM_CHOC_INCLUDE}"
        "${_PULP_WEBUI_ROOT}/build/_deps/choc-src"
        "${_PULP_WEBUI_ROOT}/../pulp/build/_deps/choc-src")
        if(_cand AND EXISTS "${_cand}/choc/text/choc_JSON.h")
            set(PULP_WEBUI_CHOC_INCLUDE "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT PULP_WEBUI_CHOC_INCLUDE OR NOT EXISTS "${PULP_WEBUI_CHOC_INCLUDE}/choc/text/choc_JSON.h")
    message(FATAL_ERROR
        "PulpWebUi: choc headers not found. Pass -DPULP_WEBUI_CHOC_INCLUDE=<dir containing choc/>.")
endif()

# ── Yoga (flex/grid layout engine) ────────────────────────────────────────────
# Same pin as tools/cmake/PulpDependencies.cmake. -DPULP_WEBUI_YOGA_DIR=<yoga
# checkout> skips the fetch (offline / cached CI).
if(NOT TARGET yogacore)
    include(FetchContent)
    if(PULP_WEBUI_YOGA_DIR)
        FetchContent_Declare(yoga SOURCE_DIR "${PULP_WEBUI_YOGA_DIR}" SOURCE_SUBDIR yoga)
    else()
        FetchContent_Declare(yoga
            GIT_REPOSITORY https://github.com/facebook/yoga.git
            GIT_TAG v3.2.1
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR yoga)
    endif()
    FetchContent_MakeAvailable(yoga)
endif()

# ── Bundled fonts ─────────────────────────────────────────────────────────────
# The Skia prebuilt does not export SkFontMgr_New_Custom_Data, so core/canvas
# materialises Inter / JetBrains Mono from embedded blobs through the platform
# font manager (bundled_fonts.cpp). There is no platform font manager in a
# browser, so these blobs ARE the font stack — without them every Label shapes
# to zero width.
include("${_PULP_WEBUI_ROOT}/tools/cmake/PulpEmbedData.cmake")
if(NOT TARGET pulp-bundled-fonts)
    pulp_add_binary_data(pulp-bundled-fonts
        SOURCES
            ${_PULP_WEBUI_ROOT}/external/fonts/Inter-Regular.ttf
            ${_PULP_WEBUI_ROOT}/external/fonts/JetBrainsMono-Regular.ttf
        NAMESPACE pulp_bundled_fonts
    )
endif()

set(_PULP_WEBUI_INCLUDES
    ${_PULP_WEBUI_ROOT}/core/platform/include
    ${_PULP_WEBUI_ROOT}/core/runtime/include
    ${_PULP_WEBUI_ROOT}/core/events/include
    ${_PULP_WEBUI_ROOT}/core/signal/include
    ${_PULP_WEBUI_ROOT}/core/audio/include
    ${_PULP_WEBUI_ROOT}/core/state/include
    ${_PULP_WEBUI_ROOT}/core/midi/include
    ${_PULP_WEBUI_ROOT}/core/canvas/include
    ${_PULP_WEBUI_ROOT}/core/render/include
    ${_PULP_WEBUI_ROOT}/core/view/include
    ${_PULP_WEBUI_ROOT}/external/nanosvg
    ${PULP_WEBUI_CHOC_INCLUDE}
)

# Runtime: the Skia-free leaf TUs canvas/view actually reach for (logging,
# tracing, identity). No crypto, no HTTP, no sockets, no threads.
set(_PULP_WEBUI_RUNTIME_SOURCES
    ${_PULP_WEBUI_ROOT}/core/runtime/src/runtime.cpp
    ${_PULP_WEBUI_ROOT}/core/runtime/src/trace.cpp
    ${_PULP_WEBUI_ROOT}/core/runtime/src/identity.cpp
    ${_PULP_WEBUI_ROOT}/core/runtime/src/scoped_no_alloc.cpp
    ${_PULP_WEBUI_ROOT}/core/runtime/src/high_resolution_timer.cpp
)

# Canvas: the full 2D drawing + text-shaping stack. bidi.cpp compiles to the
# LTR pass-through without SheenBidi; font_registry_stubs.cpp stays in because
# bundled_fonts.cpp implements only the Skia half of the registration API.
set(_PULP_WEBUI_CANVAS_SOURCES
    ${_PULP_WEBUI_ROOT}/core/canvas/src/color.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/recording_canvas.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/svg.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/scene.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/text_layout.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/attributed_string.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/text_shaper.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/text_run_planner.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/text_font_context.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/bidi.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/emoji_segmenter.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/font_options.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/font_scope.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/font_resolver.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/font_registry_stubs.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/font_flight_recorder.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/bundled_fonts.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/noto_color_emoji_stub.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/sdf_atlas.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/msdf_atlas.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/sdf_atlas_cache.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/path_to_sdf.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/image_codecs_gif.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/image_codecs_tiff.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/path.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/image_file_cache.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_path.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_fonts.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_text.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_filter.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_mask.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_gradients.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_box_shadow.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_opacity.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/skia_canvas_shaders.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/box_shadow_cache.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/svg_dom_cache.cpp
    ${_PULP_WEBUI_ROOT}/core/canvas/src/lottie_animation.cpp
)

# View: the widget + layout + theme core. NO scripting, NO design-import
# authoring, NO window hosts other than the browser one below.
set(_PULP_WEBUI_VIEW_SOURCES
    ${_PULP_WEBUI_ROOT}/core/view/src/view.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/view_paint.cpp
    # FU-2 partial-repaint damage model — pure, no platform deps. Kept in the
    # web source set so the view lib links identically across native + wasm.
    ${_PULP_WEBUI_ROOT}/core/view/src/repaint_damage.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/value_source_binding.cpp
    # The text editor was split into several TUs; the wasm build needs the same set the
    # native build compiles (core/view/CMakeLists.txt), because a Label can open the editor's
    # default context menu (label.cpp → TextEditor::show_default_context_menu) and that pulls
    # in the editor's model, clipboard, IME, and paint TUs plus its vtable.
    ${_PULP_WEBUI_ROOT}/core/view/src/text_edit_model.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/text_editor.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/text_editor_clipboard.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/text_editor_ime.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/text_editor_paint.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/slider_core.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/context_menu.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/yoga_layout.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/grid_layout.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/layout_snapshot.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/pointer_dispatch.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/gesture.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/caret.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/continuous_frames.cpp
    # continuous_frames.cpp does a dynamic_cast<EqCurveView*> (a hover-settle
    # animation branch), which references EqCurveView's RTTI typeinfo. Its key
    # function lives in eq_curve_view.cpp, so that translation unit must be in
    # every target that compiles continuous_frames.cpp or the WASM/wasm-ld link
    # fails with "undefined symbol: typeinfo for pulp::view::EqCurveView". The
    # native view lib already lists it (core/view/CMakeLists.txt); this WebUI
    # source list was missed when the EQ curve editor landed.
    ${_PULP_WEBUI_ROOT}/core/view/src/eq_curve_view.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/frame_clock.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/motion.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/motion_geometry.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/motion_cost.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/motion_preferences.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/theme.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/theme_presets.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/theme_contrast.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/appearance_tracker.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/w3c_tokens.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/css_gradient.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/control_painters.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widget_skin_derive.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widgets.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widgets/label.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widgets/visualizers.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widgets/svg_rect.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/widgets/svg_line.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/svg_fragment.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/svg_path_widget.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/accessibility.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/accessibility_tree.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/text_accessibility.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/image_cache.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/asset_manager.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/screenshot_stub.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/screenshot_skia.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/screenshot_compare.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/window_host_stub.cpp
    ${_PULP_WEBUI_ROOT}/core/view/src/ui_components.cpp
    # WindowHost's KEY FUNCTION. Its out-of-line destructor lives here, so this TU
    # is where the compiler emits WindowHost's vtable and typeinfo — without it the
    # browser host fails to link with `undefined symbol: typeinfo for
    # pulp::view::WindowHost`, which reads like an RTTI flag problem and is not one.
    # The file is otherwise about NativeViewHost, which the browser does not use;
    # it is here for the base-class symbols alone.
    ${_PULP_WEBUI_ROOT}/core/view/src/native_view_host.cpp
)

# Render: the Ganesh surface + the rAF render loop. render_loop.cpp carries the
# cross-platform RenderLoop base; render_loop_emscripten.cpp is its rAF backend.
set(_PULP_WEBUI_RENDER_SOURCES
    ${_PULP_WEBUI_ROOT}/core/render/src/skia_surface_ganesh.cpp
    ${_PULP_WEBUI_ROOT}/core/render/src/render_loop.cpp
    ${_PULP_WEBUI_ROOT}/core/render/src/render_loop_emscripten.cpp
    ${_PULP_WEBUI_ROOT}/core/render/src/headless_surface.cpp
)

# Events: only the main-thread dispatcher the browser host registers a backend
# with. No EventLoop, no Timer, no threads.
set(_PULP_WEBUI_EVENTS_SOURCES
    ${_PULP_WEBUI_ROOT}/core/events/src/main_thread_dispatcher.cpp
    # StateStore::set_main_loop makes store.cpp emit a call to EventLoop::dispatch, so the
    # store needs the event loop compiled in.
    ${_PULP_WEBUI_ROOT}/core/events/src/event_loop.cpp
)

# The parameter StateStore. The REAL plugin editor (not the generated grid) reads its params
# from a live StateStore the web host owns and the JS side syncs — so this build needs the
# store itself, NOT the DSP that usually owns it. store.cpp pulls state_migration.cpp in for
# the schema-version table it holds by value.
set(_PULP_WEBUI_STATE_SOURCES
    ${_PULP_WEBUI_ROOT}/core/state/src/store.cpp
    ${_PULP_WEBUI_ROOT}/core/state/src/state_migration.cpp
)

# The browser WindowHost (canvas input events -> View tree, GL surface -> paint).
file(GLOB _PULP_WEBUI_PLATFORM_SOURCES
    "${_PULP_WEBUI_ROOT}/core/view/platform/web/*.cpp")

# The browser clipboard (navigator.clipboard write + paste-event capture). It backs the text
# editor a selectable/editable Label opens; without it the wasm link fails on
# pulp::platform::Clipboard::{set_text,get_text,has_text}. Emscripten-guarded, so it is inert
# in any non-wasm compile.
list(APPEND _PULP_WEBUI_PLATFORM_SOURCES
    "${_PULP_WEBUI_ROOT}/core/platform/platform/web/clipboard_web.cpp")

set(_PULP_WEBUI_ALL_SOURCES
    ${_PULP_WEBUI_RUNTIME_SOURCES}
    ${_PULP_WEBUI_EVENTS_SOURCES}
    ${_PULP_WEBUI_STATE_SOURCES}
    ${_PULP_WEBUI_CANVAS_SOURCES}
    ${_PULP_WEBUI_VIEW_SOURCES}
    ${_PULP_WEBUI_RENDER_SOURCES}
    ${_PULP_WEBUI_PLATFORM_SOURCES}
)

# Fail loud and specific when a cross-workstream source has not landed yet:
# a missing TU otherwise surfaces as a wall of undefined symbols at link.
set(_PULP_WEBUI_MISSING "")
foreach(_src IN LISTS _PULP_WEBUI_RENDER_SOURCES)
    if(NOT EXISTS "${_src}")
        list(APPEND _PULP_WEBUI_MISSING "${_src}")
    endif()
endforeach()
if(NOT _PULP_WEBUI_PLATFORM_SOURCES)
    list(APPEND _PULP_WEBUI_MISSING
        "${_PULP_WEBUI_ROOT}/core/view/platform/web/ (browser WindowHost)")
endif()
if(_PULP_WEBUI_MISSING)
    string(REPLACE ";" "\n    " _missing_str "${_PULP_WEBUI_MISSING}")
    message(FATAL_ERROR
        "PulpWebUi: the browser render backend is not present in this checkout:\n"
        "    ${_missing_str}\n"
        "  The Ganesh surface, the requestAnimationFrame RenderLoop, and the "
        "browser WindowHost are what make a wasm UI module possible.")
endif()

# The C ABI the browser UI module exports. KEEP IN SYNC WITH:
#   1. examples/web-demos/super-convolver-ui/ui_entry.cpp (the definitions)
#   2. this list (the Emscripten export table)
#   3. examples/web-demos/super-convolver-ui/pulp-ui.js (mountPulpUi) and
#      examples/web-demos/super-convolver-ui/browser-test/validate.mjs, whose
#      EXPECTED_EXPORTS check fails the fixture when these three drift apart.
# UI -> host is NOT here: it leaves through EM_JS calls into Module.onParamChange
# / onGestureBegin / onGestureEnd.
set(_PULP_WEBUI_EXPORTED_FUNCTIONS
    "['_malloc','_free','_pulp_ui_add_param','_pulp_ui_init','_pulp_ui_resize','_pulp_ui_set_param','_pulp_ui_get_param','_pulp_ui_repaint','_pulp_ui_gpu_available','_pulp_ui_widget_rect','_pulp_ui_capture_png','_pulp_ui_shutdown']")

# pulp_add_web_ui(<Name> ENTRY <entry.cpp> [INCLUDES <dir>...] [ASSETS <file>...])
#
# NO -pthread: the UI subset is single-threaded by construction (the render loop
# is requestAnimationFrame, not a worker thread). A TU that pulls in std::thread
# aborts at runtime in a non-pthread module — link it out, don't paper over it.
function(pulp_add_web_ui NAME)
    cmake_parse_arguments(ARG "" "ENTRY" "INCLUDES;ASSETS" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_web_ui(${NAME}): ENTRY <file.cpp> is required.")
    endif()

    add_executable(${NAME} ${ARG_ENTRY} ${_PULP_WEBUI_ALL_SOURCES})
    target_include_directories(${NAME} PRIVATE
        ${_PULP_WEBUI_INCLUDES}
        ${SKIA_INCLUDE_DIRS}
        ${ARG_INCLUDES})
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    target_compile_definitions(${NAME} PRIVATE
        PULP_HAS_SKIA=1
        PULP_HAS_TEXT_SHAPING=1
        PULP_HAS_YOGA=1
        PULP_ENABLE_JS=0
        # This build compiles AND arms the rAF RenderLoop (render_loop.cpp +
        # render_loop_emscripten.cpp above; window_host_web starts it). Without this macro,
        # WindowHost::schedule_repaint() cannot see the loop and falls back to a SYNCHRONOUS
        # repaint() — so a view that calls request_repaint() during paint (any continuously
        # animating editor, e.g. SuperConvolver's living field) re-enters paint immediately
        # and recurses until the JS stack overflows. Native defines this on pulp-view-core;
        # the hand-listed wasm build must define it too.
        PULP_VIEW_HAS_RENDER_LOOP=1)
    # NOTE: SK_TRIVIAL_ABI=[[clang::trivial_abi]] is REQUIRED against the wasm
    # Skia slice (it is built with gn `is_trivial_abi = true`; without the macro
    # wasm-ld silently links a trapping stub for every cross-boundary sk_sp
    # call). It is set as an INTERFACE definition on skia::skia in
    # tools/cmake/FindSkia.cmake so every wasm consumer inherits it — do not
    # re-declare it here.
    target_compile_options(${NAME} PRIVATE -O2)
    target_link_libraries(${NAME} PRIVATE skia::skia yogacore pulp-bundled-fonts)

    set(_link
        "-O2"
        "-sALLOW_MEMORY_GROWTH=1"
        # GROWABLE_ARRAYBUFFERS (Emscripten's default) backs the heap with a
        # RESIZABLE ArrayBuffer. WebGL rejects a TypedArray over a resizable
        # buffer ("The provided ArrayBufferView value must not be resizable"),
        # and Skia's Ganesh backend uploads vertex/index data straight from the
        # heap — so every glBufferSubData throws on the first frame. Turning it
        # off keeps memory growth (via copy) and keeps the WebGL uploads legal.
        "-sGROWABLE_ARRAYBUFFERS=0"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web"
        "-sMAX_WEBGL_VERSION=2"
        "-sMIN_WEBGL_VERSION=2"
        "-sEXPORTED_FUNCTIONS=${_PULP_WEBUI_EXPORTED_FUNCTIONS}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToNewUTF8','HEAPU8','HEAPF32','HEAP32']"
        "--no-entry"
    )
    # icudtl.dat — SkUnicode's ICU data. core/canvas loads it through the normal
    # filesystem API, so it has to exist in MEMFS at the same path.
    if(SKIA_ICUDTL_FILE AND EXISTS "${SKIA_ICUDTL_FILE}")
        list(APPEND _link "--preload-file" "${SKIA_ICUDTL_FILE}@/icudtl.dat")
    endif()
    foreach(_asset IN LISTS ARG_ASSETS)
        get_filename_component(_asset_name "${_asset}" NAME)
        list(APPEND _link "--preload-file" "${_asset}@/${_asset_name}")
    endforeach()

    string(JOIN " " _link_str ${_link})
    set_target_properties(${NAME} PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".js"
        LINK_FLAGS "${_link_str}")
endfunction()
