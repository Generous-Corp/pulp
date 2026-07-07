# View widgets, inspector, WidgetBridge, text editor, layout, and visual harness tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
pulp_add_test_suite(pulp-test-platform-maturity LIBRARIES pulp::view)

# Audio bridge tests
pulp_add_test_suite(pulp-test-audio-bridge LIBRARIES pulp::view)
# Widget tests
pulp_add_test_suite(pulp-test-widgets LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-parameter-edit LIBRARIES pulp::view pulp::state)
pulp_add_test_suite(pulp-test-param-host-sync LIBRARIES pulp::state INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/core/format/include)
pulp_add_test_suite(pulp-test-midi-binding LIBRARIES pulp::view pulp::state pulp::midi)
# Widget tests — Label cluster extracted from test_widgets.cpp. Label
# intrinsic_width / intrinsic_height /
# line-height multiplier / line_clamp / measured_height under
# bounded width / baseline_y from text metrics / vertical text
# direction / letter_spacing glyph counting.
pulp_add_test_suite(pulp-test-widgets-label LIBRARIES pulp::view)
# Hot-reload tests
add_executable(pulp-test-hot-reload test_hot_reload.cpp)
target_link_libraries(pulp-test-hot-reload PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow`: each HotReloader scenario sleeps on file-watcher
# debounce + filesystem mtime resolution (~1-1.5 sec each).
catch_discover_tests(pulp-test-hot-reload
    PROPERTIES
        RESOURCE_LOCK hot-reload-file-watcher
        LABELS slow)

# Non-GPU inspector helpers. The full inspector-domain suite is GPU-gated
# because it exercises View/Render integration, but these domain helpers are
# plain data/StateStore contracts and should stay covered in CPU-only builds.
add_executable(pulp-test-inspector-domain-helpers
    test_inspector_domain_helpers.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/audio_inspector.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/console_capture.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/editor_url.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/state_inspector.cpp)
target_include_directories(pulp-test-inspector-domain-helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/inspect/include)
target_link_libraries(pulp-test-inspector-domain-helpers PRIVATE
    pulp::audio pulp::canvas pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-domain-helpers)

# Inspector tests — only when GPU is enabled (pulp-inspect requires GPU stack).
if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
    add_executable(pulp-test-inspector test_inspector.cpp)
    target_link_libraries(pulp-test-inspector PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    # PULP_INSPECTOR_NO_LAUNCH: the source-jump tests exercise the J
    # hotkey, whose handler resolves with dry_run=false. Without this
    # guard the test would spawn a real `open vscode://file/...`, popping
    # a macOS open-confirmation dialog. The env var makes launch_editor_url()
    # a no-op. The test file also sets it in-process so it stays safe when
    # the binary is run directly, outside CTest.
    catch_discover_tests(pulp-test-inspector
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Inspector sibling TUs: GPU pass attribution, source-jump,
    # drift/reconcile, and atlas viewer.
    # Each mirrors pulp-test-inspector's exact registration (own executable
    # + catch_discover_tests, same libraries, same NO_LAUNCH env guard).
    add_executable(pulp-test-inspector-gpu-passes test_inspector_gpu_passes.cpp)
    target_link_libraries(pulp-test-inspector-gpu-passes PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-gpu-passes
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Wiring tab — lists design-sourced (Figma) overlays + wired/unwired badge.
    add_executable(pulp-test-inspector-wiring test_inspector_wiring.cpp)
    target_link_libraries(pulp-test-inspector-wiring PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-wiring
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-source-jump test_inspector_source_jump.cpp)
    target_link_libraries(pulp-test-inspector-source-jump PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-source-jump
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-drift-reconcile test_inspector_drift_reconcile.cpp)
    target_link_libraries(pulp-test-inspector-drift-reconcile PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-drift-reconcile
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-atlas-viewer test_inspector_atlas_viewer.cpp)
    target_link_libraries(pulp-test-inspector-atlas-viewer PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-atlas-viewer
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-server test_inspector_server.cpp)
    target_link_libraries(pulp-test-inspector-server PRIVATE pulp::inspect Catch2::Catch2WithMain)
    if(WIN32)
        catch_discover_tests(pulp-test-inspector-server
            PROPERTIES
                ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1"
                LABELS "windows-pr-quarantine")
    else()
        catch_discover_tests(pulp-test-inspector-server
            PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
    endif()

    # Additional inspector sibling TUs. Same registration as
    # pulp-test-inspector.
    add_executable(pulp-test-inspector-domains test_inspector_domains.cpp)
    target_link_libraries(pulp-test-inspector-domains PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-domains
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-field-edit test_inspector_field_edit.cpp)
    target_link_libraries(pulp-test-inspector-field-edit PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-field-edit
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-eyedropper test_inspector_eyedropper.cpp)
    target_link_libraries(pulp-test-inspector-eyedropper PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-eyedropper
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # TweakStore + Inspector.applyTweak protocol surface.
    add_executable(pulp-test-tweak-store test_tweak_store.cpp)
    target_link_libraries(pulp-test-tweak-store PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-tweak-store)

    # Editor URI plumbing for the source-jump action.
    # Pure config / format-helper / protocol — no overlay / GPU surface,
    # but lives under the same PULP_ENABLE_GPU guard as the rest of
    # pulp-inspect's tests so it links against the same library.
    add_executable(pulp-test-editor-url test_editor_url.cpp)
    target_link_libraries(pulp-test-editor-url PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-editor-url
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Agent-request queue: pure serialize/parse/append/ack + atomic file I/O.
    # No overlay / GPU surface, but links pulp::inspect so it shares the guard.
    add_executable(pulp-test-agent-request-queue test_agent_request_queue.cpp)
    target_link_libraries(pulp-test-agent-request-queue PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-agent-request-queue
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
endif()

# Widget bridge tests
set(_pulp_widget_bridge_test_libs pulp::view)
if(TARGET pulp-render)
    list(APPEND _pulp_widget_bridge_test_libs pulp::render)
endif()
pulp_add_test_suite(pulp-test-widget-bridge LIBRARIES ${_pulp_widget_bridge_test_libs})
pulp_add_test_suite(pulp-test-widget-bridge-capabilities LIBRARIES ${_pulp_widget_bridge_test_libs})

# Widget bridge — source-level API contract. Keeps JS-native registrations
# unique and matched to the reviewed bridge API manifest so future registrar
# splits cannot add unclassified bridge entries.
pulp_add_test_suite(pulp-test-widget-bridge-api-contracts
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# Widget bridge — no-GPU gate enforcement.
# Pure static scan: walks widget_bridge.cpp line-by-line and asserts every
# `gpu_surface_->` dereference is inside a PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
# gate (or the #else of an #ifndef PULP_HAS_SKIA block, which implies the
# render module's include path is present). Catches the iOS Simulator
# regression class where GpuSurface is forward-declared but its members are
# called from configures that did not link the render module. Runs in the
# default macOS lane in milliseconds — no Xcode / iOS SDK required, so it
# closes the gap the slow `cmake-ios-auv3-configure` test left behind for
# validation.
pulp_add_test_suite(pulp-test-widget-bridge-no-gpu-gates
    COMPILE_DEFINITIONS PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Widget bridge — runtime-import handlers.
pulp_add_test_suite(pulp-test-widget-bridge-runtime-import LIBRARIES pulp::view)

# Widget bridge — declarative native→widget param/meter bindings
# (bindWidgetToParam / bindMeter / unbindWidget + gesture precedence).
pulp_add_test_suite(pulp-test-widget-bridge-param-binding LIBRARIES pulp::view pulp::state)

# Widget bridge — Canvas2D surface. Covers canvasSetTransform /
# canvasClip / canvasGlobalCompositeOperation, canvasMeasureText /
# canvasSetLineDash / canvasDrawImage, canvasGetImageData /
# canvasPutImageData, and 4-arg canvasFillText prior-state preservation.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d LIBRARIES pulp::view)

# Widget bridge — Yoga layer. Three coherent clusters: (1) dimension
# percent strings (width/height + min/max via Yoga's percent API),
# (2) flexBasis% promotion (basis "NN%" + "flex 1 1 NN%"
# decomposition), and (3) yoga value-aliasing
# (flexDirection / justifyContent / alignItems / alignSelf / order /
# flexWrap value translations).
pulp_add_test_suite(pulp-test-widget-bridge-yoga LIBRARIES pulp::view)

# Widget bridge — recovered Canvas2D/CSS compatibility regressions.
# The canonical Canvas2D bridge surface is in
# pulp-test-widget-bridge-wave2-cheap below; this older split keeps
# recovered CSS cases and later Canvas2D bridge regressions.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-wave2 LIBRARIES pulp::view)

# Widget bridge — yoga logical-edge fan-out + A4 OOS pins. Covers yoga
# logical-edge marginInline/Block + paddingInline/Block + inset
# shorthand plus CSS NOT-IMPL closure catalog hygiene.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-a4-oos LIBRARIES pulp::view)

# Widget bridge — compatibility cheap-wiring. Bundles the Canvas2D
# wiring (DIVERGE → PASS) + CSS value-coverage entries from compat.json.
pulp_add_test_suite(pulp-test-widget-bridge-wave2-cheap LIBRARIES pulp::view)

# Widget bridge — RN style-prop bridge primitives.
# setShadow / setOpacity / setTransform RN-shaped style
# functions flow through bridge into View's slots. Includes RN's
# shadowOpacity-into-color-alpha composition + transform-prop
# aggregation.
pulp_add_test_suite(pulp-test-widget-bridge-rn-style LIBRARIES pulp::view)

# Widget bridge — Tier-4 OOS / perf-hints / interaction misc pins.
# Pins the no-op / fallback contract for properties
# Pulp deliberately doesn't paint: 3D transforms, generated content,
# scroll-snap, will-change, contain, touch-action
# secondary keywords. Catalog hygiene tests.
pulp_add_test_suite(pulp-test-widget-bridge-tier4-oos LIBRARIES pulp::view)

# Widget bridge — RN outline cluster.
# outlineColor / outlineOffset / outlineStyle / outlineWidth longhands
# + `outline` shorthand decomposition through the RN style shim.
pulp_add_test_suite(pulp-test-widget-bridge-rn-outline LIBRARIES pulp::view)

# Widget bridge — clip-path + mask cluster.
# clip-path: inset/circle/polygon/url + shape coordinate parsing;
# mask-image: url + linear-gradient + transforms; mask-size /
# -position / -repeat / -origin / -clip / -composite; mask shorthand.
pulp_add_test_suite(pulp-test-widget-bridge-clip-mask LIBRARIES pulp::view)

# Widget bridge — SVG widgets. Three clusters: SvgPathWidget JS bridge
# integration, SvgRectWidget + SvgLineWidget JS bridge integration,
# and compound-path
# parser regression (Spectr PEAK / AVG / BOTH / OFF analyzer icons).
pulp_add_test_suite(pulp-test-widget-bridge-svg LIBRARIES pulp::view)

# Widget bridge — CSS compatibility audit. Runtime-path coverage for
# 49 entries flipped from partial/DIVERGE to supported:
# backgroundPosition / backgroundSize / textShadow /
# border / border-side / borderRadius / per-corner radius / boxShadow
# / opacity / outline / textOverflow / transformOrigin / zIndex /
# backdropFilter / display / overflow / overflow per-axis / and
# many more. 45 TEST_CASEs each exercising JS shim → bridge → View
# slot round-trip.
pulp_add_test_suite(pulp-test-widget-bridge-wave5-css LIBRARIES pulp::view)

# Widget bridge — HTML ARIA + querySelector. aria-label / role flow
# into View accessibility slots;
# document.querySelector accepts attribute selectors + combinators +
# :hover / :disabled / :checked / :enabled / :not / :first-child /
# :nth-child / :empty pseudo-classes.
pulp_add_test_suite(pulp-test-widget-bridge-html-aria LIBRARIES pulp::view)

# Widget bridge — CSS animations + transitions.
# animation-* longhands + shorthand decomposition; transition-*
# longhands + shorthand round-trip through the CSS shim and bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-animations LIBRARIES pulp::view)

# Widget bridge — CSS Grid extended surface.
# grid-template-columns / -rows / -areas, grid-column / -row / -area
# placement shorthand, gap longhands + shorthand, justify-* / align-* /
# place-* alignment, repeat() + minmax() + fr-unit + auto sizing tokens
# round-trip through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-grid LIBRARIES pulp::view)

# Widget bridge — animation API cluster. Bridge ↔ MotionEngine
# plumbing for setMotionToken, animate(), the Web
# Animations API surface (Element.animate / KeyframeEffect), motion
# provenance, and pulp-motion-bench harness output. Self-contained
# ~800-line cluster from test_widget_bridge.cpp.
pulp_add_test_suite(pulp-test-widget-bridge-animation LIBRARIES pulp::view)

# Widget bridge — per-edge margin / padding. marginTop /
# marginRight / marginBottom / marginLeft (and padding counterparts)
# each route to their own Yoga edge enum without cross-contamination
# through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-per-edge LIBRARIES pulp::view)

# Widget bridge — RN-OOS-fixup catalog audit tail. Material elevation
# shim, includeFontPadding round-trip, borderCurve
# squircle paint dispatch, isolation honest CSS-subset, and other
# RN-side OOS catalog hygiene checks.
pulp_add_test_suite(pulp-test-widget-bridge-rn-oos-fixup LIBRARIES pulp::view)

# Widget bridge — Canvas2D bridge-fn cluster. canvasSetFontFull,
# fillRule, and canvasSetDirection / canvasSetFilter are closely related
# bridge entry-points that thread JS-side Canvas2D semantics through
# WidgetBridge into the native canvas pipeline.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-bridge-fns LIBRARIES pulp::view)

# Widget bridge — Yoga borderWidth wiring.
# Pins YGNodeStyleSetBorder integration with Yoga 3.x default box-sizing
# (border-box); the content-box case belongs with setBoxSizing coverage.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-border LIBRARIES pulp::view)

# Widget bridge — CSS-misc cluster. Text-decoration longhands,
# line-clamp, and background-repeat are two coherent
# small CSS clusters that keep test_widget_bridge.cpp under the
# 3,000-line target.
pulp_add_test_suite(pulp-test-widget-bridge-css-misc LIBRARIES pulp::view)

# Autonomous Spectr regression suite. Composes the six
# [contract] invariants from
# test_widget_bridge.cpp into a Spectr-shaped mini-scenario so a future
# change that keeps each unit-level invariant passing but breaks their
# *interaction* still fails first.
pulp_add_test_suite(pulp-test-spectr-regression LIBRARIES pulp::view)

# Typography inheritance for the CSS-style cascade
pulp_add_test_suite(pulp-test-typography-inheritance LIBRARIES pulp::view)

# Text-overflow ellipsis helper
pulp_add_test_suite(pulp-test-text-overflow LIBRARIES pulp::view)

# Editor bridge tests for the renderer-agnostic envelope/dispatcher
pulp_add_test_suite(pulp-test-editor-bridge LIBRARIES pulp::view)

# Input events tests
pulp_add_test_suite(pulp-test-input-events LIBRARIES pulp::view)

# Text editor tests
pulp_add_test_suite(pulp-test-text-editor LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-mouse LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)
pulp_add_test_suite(pulp-test-text-editor-paint LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-policy LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)

# TextEditor multi-line coverage: wrap, click-to-caret in
# wrapped rows, caret_rect pixel positioning, single-vs-multi Enter
# contract). Keeps the original test_text_editor.cpp file unchanged
# so the existing single-line surface stays pinned in isolation.
pulp_add_test_suite(pulp-test-text-editor-multiline LIBRARIES pulp::view)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
pulp_add_test_suite(pulp-test-text-input LIBRARIES pulp::view)

# W3C Layout parity tests (flexbox, box model, visual properties)
pulp_add_test_suite(pulp-test-layout-w3c LIBRARIES pulp::view)

# W3C design-token runtime pair (parse/export) via the self-contained
# w3c_tokens.hpp — stays always-compiled when design-import is gated.
pulp_add_test_suite(pulp-test-w3c-tokens LIBRARIES pulp::view)

# LottieView playback logic (opt-in PULP_LOTTIE). Passes whether or not Lottie
# is compiled in: LottieView::supported() gates the playback assertions.
pulp_add_test_suite(pulp-test-lottie-view LIBRARIES pulp::view)

# Visual semantic snapshot harness. This is a custom-main Catch2 binary:
# --self-test runs its focused tests, while --fixture emits a stable JSON
# snapshot for tools/harness/visual/runner.py.
add_executable(pulp-test-visual visual/pulp-test-visual.cpp)
target_link_libraries(pulp-test-visual PRIVATE pulp::view Catch2::Catch2)
add_test(NAME visual-harness-self-test COMMAND pulp-test-visual --self-test)
set_tests_properties(visual-harness-self-test PROPERTIES
    LABELS "visual;harness;yoga"
    TIMEOUT 30)
