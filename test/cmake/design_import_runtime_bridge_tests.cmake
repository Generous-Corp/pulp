# Design import runtime bridge test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# backend (no GPU window).
add_executable(pulp-test-image-view-fill
    test_image_view_fill.cpp)
target_link_libraries(pulp-test-image-view-fill
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-view-fill
    PROPERTIES LABELS "view")

# GPU regression for the same silhouette fill on the LIVE Graphite path: the
# url() mask shader must upload its image to a GPU texture or Graphite drops the
# masked draw every frame (ELYSIUM "shapes don't fill" bug). macOS-only; the
# offscreen GPU backend soft-skips at runtime when no Dawn adapter is present.
if(APPLE)
    add_executable(pulp-test-image-view-fill-gpu
        test_image_view_fill_gpu.cpp)
    target_link_libraries(pulp-test-image-view-fill-gpu
        PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-image-view-fill-gpu
        PROPERTIES LABELS "view")
endif()

# DesignFrameView (Plan B / B1) — faithful SVG render + typed interactive knobs.
add_executable(pulp-test-design-frame-view
    test_design_frame_view.cpp)
target_link_libraries(pulp-test-design-frame-view
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-frame-view
    PROPERTIES LABELS "view")

# HostParamSurface / HostActionSurface — the SDK runtime host-param + action
# surfaces: StateStore backing, DesignFrameView
# sync/route/re-key, action forwarding, and cross-host binding via a fake.
# The rt_allocation_probe harness intercepts global operator new, so the
# paint-safe display-text reads can be asserted alloc-free rather than merely
# documented as such.
add_executable(pulp-test-host-param-surface
    test_host_param_surface.cpp
    harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-host-param-surface
    PRIVATE pulp::view pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-host-param-surface
    PROPERTIES LABELS "view")

# Faithful port toolkit — SDK primitives
# (SVG fragment handles, anchored popover, drag-to-reorder, paint-space painters).
add_executable(pulp-test-faithful-port-toolkit
    test_faithful_port_toolkit.cpp)
target_include_directories(pulp-test-faithful-port-toolkit PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-faithful-port-toolkit
    PRIVATE pulp::view pulp-annotated-capture Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-faithful-port-toolkit
    PROPERTIES LABELS "view")

# MusicalTypingKeyboard — Ink & Signal catalog component (faithful Figma SVG
# via DesignFrameView). Pins SVG load, headless render, catalog registration.
add_executable(pulp-test-musical-typing-keyboard
    test_musical_typing_keyboard.cpp)
target_link_libraries(pulp-test-musical-typing-keyboard
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-musical-typing-keyboard
    PROPERTIES LABELS "view")

# ChannelStripView — faithful Figma-vector catalog component.
add_executable(pulp-test-channel-strip-view
    test_channel_strip_view.cpp)
target_link_libraries(pulp-test-channel-strip-view
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-channel-strip-view
    PROPERTIES LABELS "view")

# Faithful Figma-vector specimen catalog components (generated).
add_executable(pulp-test-faithful-specimens
    test_faithful_specimens.cpp)
target_link_libraries(pulp-test-faithful-specimens
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-faithful-specimens
    PROPERTIES LABELS "view")

# W3C Design Tokens cluster extracted from test_design_import.cpp.
# Covers parse_w3c_tokens, export_w3c_tokens,
# composite typography/shadow shapes, alias resolution, math
# expressions, group $type inheritance, ir_tokens_to_theme +
# theme_to_ir_tokens round-trips.
add_executable(pulp-test-design-import-w3c-tokens test_design_import_w3c_tokens.cpp)
target_link_libraries(pulp-test-design-import-w3c-tokens PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-w3c-tokens PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-w3c-tokens
    PROPERTIES LABELS "parser-import")

# Baked SwiftUI emitter. Golden-string asserts plus a swiftc type-check
# gate (find_program; the test skips when swiftc / the SwiftUI SDK is
# unavailable, e.g. the Linux lane). Registered here, outside the
# planning-artifact-guarded cpp block above, since it needs no
# generated artifacts.
find_program(PULP_SWIFTC swiftc)
add_executable(pulp-test-design-swift-codegen test_design_swift_codegen.cpp)
target_link_libraries(pulp-test-design-swift-codegen
    PRIVATE pulp::view-core pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-swift-codegen PRIVATE
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    PULP_TEST_SWIFTC="${PULP_SWIFTC}")
catch_discover_tests(pulp-test-design-swift-codegen
    PROPERTIES LABELS "parser-import")

# React-runtime parser cluster extracted from test_design_import.cpp.
# Covers TSX/runtime React parsers for every
# supported design-tool source: parse_v0_tsx / parse_v0_dev_react,
# parse_figma_make_react, parse_stitch_react, parse_react_native_export,
# parse_pencil_react. Shared contract: parse fixture, materialize via
# host React shim, accept sanitized TSX, reject out-of-matrix surfaces.
add_executable(pulp-test-design-import-react-runtime test_design_import_react_runtime.cpp)
target_link_libraries(pulp-test-design-import-react-runtime PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-react-runtime PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-react-runtime
    PROPERTIES LABELS "parser-import")

# stable_anchor_id assignment for imported design nodes. PULP_REPO_ROOT lets
# the cross-language conformance cases read test/fixtures/anchor_vectors.json,
# the shared vector table the @pulp/import-ir vitest suite reads too.
add_executable(pulp-test-design-import-anchors test_design_import_anchors.cpp)
target_link_libraries(pulp-test-design-import-anchors PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-anchors PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-anchors
    PROPERTIES LABELS "parser-import")

# Inspector lock-to-source, Path A (generated-TSX/JS rewrite).
# Proves the tweak -> lock-to-source -> re-import round-trip.
add_executable(pulp-test-lock-to-source test_lock_to_source.cpp)
target_link_libraries(pulp-test-lock-to-source PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-lock-to-source
    PROPERTIES LABELS "parser-import")

# WYSIWYG T3 — ui-preview settle-probe design-viewport sizing. The probe
# algorithm lives in examples/ui-preview/design_viewport_probe.hpp (header-
# only) so it can be tested headlessly without linking the ui-preview app.
add_executable(pulp-test-ui-preview-viewport test_ui_preview_viewport.cpp)
target_link_libraries(pulp-test-ui-preview-viewport PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ui-preview-viewport
    PROPERTIES LABELS "view")

# DESIGN.md import (Google design.md, Apache-2.0)
# Compiles import_detect.cpp directly into the test target so the
# detector tests do not require linking the whole pulp-import-design CLI.
add_executable(pulp-test-design-import-designmd
    test_design_import_designmd.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/import_detect.cpp)
target_include_directories(pulp-test-design-import-designmd PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/import-design
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-design-import-designmd PRIVATE
    pulp::view
    Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-designmd PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-designmd
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Token lock-to-source via DESIGN.md rewrite.
add_executable(pulp-test-token-lock test_token_lock.cpp)
target_link_libraries(pulp-test-token-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-token-lock
    PROPERTIES LABELS "parser-import")

# Inspector lock-to-source, Path B (hand-authored JSX/TSX patch).
# Proves the tweak -> JSX/TSX surgical patch -> formatting-preserving
# round-trip, plus the ambiguous / not-found / too-dynamic failure paths.
add_executable(pulp-test-jsx-lock test_jsx_lock.cpp)
target_link_libraries(pulp-test-jsx-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-jsx-lock
    PROPERTIES LABELS "parser-import")

# Library-backed post-parse widget promotion: <div onClick> / role=button /
# cursor:pointer → button. The pass now lives in pulp::view so
# parser API users and the CLI share one normalization path.
add_executable(pulp-test-widget-promotion
    test_widget_promotion.cpp)
target_include_directories(pulp-test-widget-promotion PRIVATE
    ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-widget-promotion PRIVATE
    pulp::view
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-promotion
    PROPERTIES LABELS "parser-import")

# Web-compat preludes shipped for bundled-React imports
# (nodeType / nodeName, observer no-ops, scheduler shims). Uses
# WidgetBridge to evaluate the same prelude stack the runtime ships.
add_executable(pulp-test-web-compat-react-shims test_web_compat_react_shims.cpp)
target_link_libraries(pulp-test-web-compat-react-shims PRIVATE pulp::view Catch2::Catch2WithMain)
# Bound-pump test runs the 1M-job cap; budget headroom for slow
# CI runners and sanitizer builds. Default discover timeout is too tight
# when a regression of the bound would hang indefinitely. Other shim
# scenarios in this binary stay in fast-CI — they're cheap and
# important — so this binary is *not* labeled `slow` even though one
# of its tests pays a 1-3 sec wall cost.
catch_discover_tests(pulp-test-web-compat-react-shims
    PROPERTIES TIMEOUT 180
)
