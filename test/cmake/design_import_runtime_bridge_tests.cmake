# Design import runtime bridge test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Grouped executable for this manifest (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake): each member keeps its own registration,
# labels and properties; only the binary behind them is shared. Every grouped
# suite here compiles against pulp::view alone (PULP_REPO_ROOT and the
# widget-promotion include root become per-source properties). A suite stays on
# its own when it needs its own process or compile line: the Apple-only GPU
# silhouette fill, host-param-surface (the RT allocation probe replaces global
# operator new), faithful-port-toolkit (links pulp-annotated-capture),
# offscreen-capture-rt-contract (the RT intercept shim), design-swift-codegen
# (pulp::view-core + pulp::platform), design-import-designmd (compiles
# import_detect.cpp and runs from the source root), and
# design-import-react-runtime, which the import-validation roundtrip scripts
# and source-contracts.json build and run by its executable name.
pulp_add_test_group(pulp-test-group-design-import-bridge LIBRARIES pulp::view)

# Value-driven silhouette fill (design-import shape-fill — item 3): exercises
# ImageView::set_fill_value + the canvas url() image mask on the Skia raster
# backend (no GPU window).
pulp_add_test_suite(pulp-test-image-view-fill GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

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
pulp_add_test_suite(pulp-test-design-frame-view GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

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
pulp_add_test_suite(pulp-test-musical-typing-keyboard GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

# ChannelStripView — faithful Figma-vector catalog component.
pulp_add_test_suite(pulp-test-channel-strip-view GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

# Offscreen capture must suspend the paint no-alloc contract.
add_executable(pulp-test-offscreen-capture-rt-contract
    test_offscreen_capture_rt_contract.cpp)
target_sources(pulp-test-offscreen-capture-rt-contract PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>)
target_link_libraries(pulp-test-offscreen-capture-rt-contract
    PRIVATE pulp::view pulp::native-components Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
target_compile_definitions(pulp-test-offscreen-capture-rt-contract PRIVATE
    $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
catch_discover_tests(pulp-test-offscreen-capture-rt-contract
    PROPERTIES LABELS "view")

# Faithful Figma-vector specimen catalog components (generated).
pulp_add_test_suite(pulp-test-faithful-specimens GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

# W3C Design Tokens cluster extracted from test_design_import.cpp.
# Covers parse_w3c_tokens, export_w3c_tokens,
# composite typography/shadow shapes, alias resolution, math
# expressions, group $type inheritance, ir_tokens_to_theme +
# theme_to_ir_tokens round-trips.
pulp_add_test_suite(pulp-test-design-import-w3c-tokens GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    LABELS "parser-import")

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
pulp_add_test_suite(pulp-test-design-import-anchors GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    LABELS "parser-import")

# Inspector lock-to-source, Path A (generated-TSX/JS rewrite).
# Proves the tweak -> lock-to-source -> re-import round-trip.
pulp_add_test_suite(pulp-test-lock-to-source GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "parser-import")

# WYSIWYG T3 — ui-preview settle-probe design-viewport sizing. The probe
# algorithm lives in examples/ui-preview/design_viewport_probe.hpp (header-
# only) so it can be tested headlessly without linking the ui-preview app.
pulp_add_test_suite(pulp-test-ui-preview-viewport GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

# DESIGN.md import (Google design.md, Apache-2.0)
# Compiles import_detect.cpp directly into the test target so the
# detector tests do not require linking the whole pulp-import-design CLI.
add_executable(pulp-test-design-import-designmd
    test_design_import_designmd.cpp
    test_design_import_designmd_040.cpp
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
pulp_add_test_suite(pulp-test-token-lock GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "parser-import")

# Inspector lock-to-source, Path B (hand-authored JSX/TSX patch).
# Proves the tweak -> JSX/TSX surgical patch -> formatting-preserving
# round-trip, plus the ambiguous / not-found / too-dynamic failure paths.
pulp_add_test_suite(pulp-test-jsx-lock GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "parser-import")

# Library-backed post-parse widget promotion: <div onClick> / role=button /
# cursor:pointer → button. The pass now lives in pulp::view so
# parser API users and the CLI share one normalization path.
pulp_add_test_suite(pulp-test-widget-promotion GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}
    LABELS "parser-import")

# Keyboard navigation for a menu no trigger owns. The popup owner's roving
# cursor was reachable only through an `aria-haspopup` trigger, so a context
# menu -- summoned at coordinates, owned by nothing -- got no cursor and
# ignored arrow keys. Adoption is gated, and most of this suite is the
# negative controls for that gate: nothing open, a trigger-owned menu, two
# open menus, an empty menu, and the explicit opt-out.
pulp_add_test_suite(pulp-test-web-compat-menu-keynav GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    LABELS "view")

# Web-compat preludes shipped for bundled-React imports
# (nodeType / nodeName, observer no-ops, scheduler shims). Uses
# WidgetBridge to evaluate the same prelude stack the runtime ships.
# The bound-pump test runs the 1M-job cap; budget headroom for slow CI runners.
# Default discover timeout is too tight when a regression of the bound would
# hang indefinitely. Instrumented builds get their multiplier from the resolver
# rather than from padding baked into this number. Other shim scenarios in this
# suite stay in fast-CI — they're cheap and important — so it is *not* labeled
# `slow` even though one of its tests pays a 1-3 sec wall cost.
pulp_add_test_suite(pulp-test-web-compat-react-shims GROUP pulp-test-group-design-import-bridge
    LIBRARIES pulp::view
    TIMEOUT 180)
