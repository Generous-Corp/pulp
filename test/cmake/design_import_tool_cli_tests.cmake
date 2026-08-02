# Design import tool and CLI test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Browser-solved HTML capture discovery, subprocess argv, cleanup, and artifact
# freshness. The launcher fixture behaves like the configured Node executable,
# so these tests exercise ChildProcess without depending on a real browser.
add_executable(pulp-browser-capture-launcher-fixture
    fixtures/browser_capture_launcher_fixture.cpp)
add_executable(pulp-test-browser-capture-backend
    test_browser_capture_backend.cpp
    # Node.js discovery: PATH plus the standard install locations, so a
    # GUI-launched app (minimal PATH) resolves the same Node a terminal does.
    test_node_runtime.cpp)
target_link_libraries(pulp-test-browser-capture-backend PRIVATE
    pulp::browser-capture-backend
    Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-browser-capture-backend PRIVATE
    PULP_BROWSER_CAPTURE_FIXTURE_PATH="$<TARGET_FILE:pulp-browser-capture-launcher-fixture>")
add_dependencies(pulp-test-browser-capture-backend
    pulp-browser-capture-launcher-fixture)
catch_discover_tests(pulp-test-browser-capture-backend
    PROPERTIES LABELS "parser-import;browser-capture")

# Dependency-free Node capture helpers: staged-root containment, tokenized
# loopback serving, snapshot redaction, network policy, and profile cleanup.
if(_PULP_NODE_FOR_TESTS)
    file(GLOB _PULP_BROWSER_CAPTURE_NODE_TESTS CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture/*.test.mjs)
    add_test(NAME pulp-browser-capture-node-unit
             COMMAND ${_PULP_NODE_FOR_TESTS} --test
                     ${_PULP_BROWSER_CAPTURE_NODE_TESTS})
    set_tests_properties(pulp-browser-capture-node-unit PROPERTIES
        TIMEOUT 180
        LABELS "parser-import;browser-capture;node")
endif()

# Pure capture-envelope lowering and HTML-shape dispatch. These stay separate
# from the subprocess/backend target so protocol validation and intake policy
# can be tested without launching Chromium.
add_executable(pulp-test-browser-capture-import
    test_browser_capture_backdrop_filter.cpp
    test_browser_capture_ir.cpp
    test_browser_capture_text_metrics.cpp
    test_browser_capture_tree.cpp
    test_browser_import_cli.cpp
    test_html_intake.cpp
    test_html_project_stager.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_import_cli.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_import_session.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture_ir.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture_styles.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture_tree.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture_validation.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_html_import.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/browser_capture_workspace.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/claude_html_dependencies.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/html_project_stager.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/html_intake.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/sprite_skins.cpp)
target_include_directories(pulp-test-browser-capture-import PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/miniz)
# A real Chromium capture of a panel built from oklab colours, gradients, and
# layered shadows. Kept byte-verbatim (the envelope carries the screenshot's
# sha256) so the computed-style lowering is proven against what Chrome actually
# serializes rather than against hand-written JSON that agrees with the parser.
# Three more real Chromium captures, one per shape where the emitted tree's
# clip and CSS's clip can disagree: a node escaping an `overflow: hidden`
# ancestor along the containing-block chain, a node hoisted out from under the
# ancestor that clips it, and a transformed subtree where neither happens. Each
# keeps the source HTML next to the capture so the document under test is
# readable without replaying the browser. Two more cover the value forms a
# stylesheet reaches paint through rather than the tree's shape: generated
# `content` on ::before / ::after, and a non-blur `backdrop-filter` list.
target_compile_definitions(pulp-test-browser-capture-import PRIVATE
    PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/test/fixtures/browser-capture-computed-style"
    PULP_BROWSER_CAPTURE_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/test/fixtures")
target_link_libraries(pulp-test-browser-capture-import PRIVATE
    pulp::browser-capture-backend
    pulp::view
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-browser-capture-import
    PROPERTIES LABELS "parser-import;browser-capture")

# Claude Design bundle envelope parser (base64+gzip JSON
# envelope unpacking, template script-order resolution).
add_executable(pulp-test-design-import-claude-bundle test_design_import_claude_bundle.cpp)
target_link_libraries(pulp-test-design-import-claude-bundle
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-claude-bundle
    PROPERTIES LABELS "parser-import")

# Keyboard-shortcut extraction from React source (UX best-practice
# default — design-import emits a shortcuts manifest the runtime can
# auto-register).
add_executable(pulp-test-design-import-shortcuts test_design_import_shortcuts.cpp)
target_link_libraries(pulp-test-design-import-shortcuts
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-shortcuts
    PROPERTIES LABELS "parser-import")

# Design-tool platform key wire-up E2E. Pins the
# `WindowHost → root.on_global_key → bridge.forward_key_event →
# registerShortcut + __dispatch__('__global__','keydown',...)` chain that
# every auto-bound default chord (and Spectr's mode-switch) depends on.
add_executable(pulp-test-platform-key-wireup test_platform_key_wireup.cpp)
target_link_libraries(pulp-test-platform-key-wireup
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform-key-wireup
    PROPERTIES LABELS "parser-import")

# Pin WidgetBridge::dispatch_global_key fan-out
# across the static all_bridges_ registry. Every live bridge gets the
# key without any per-app on_global_key wiring; this is the framework
# contract platform hosts depend on.
add_executable(pulp-test-widget-bridge-dispatch-global-key
    test_widget_bridge_dispatch_global_key.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-global-key
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-global-key)

# Pin WidgetBridge::dispatch_document_event and the real
# document.addEventListener. Platform hosts fire synthetic
# outside-click events on Esc through this path so React popovers
# (Spectr's PickerDropdown, ContextMenu, etc.) close automatically.
add_executable(pulp-test-widget-bridge-dispatch-document-event
    test_widget_bridge_dispatch_document_event.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-document-event
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-document-event)

# `pulp import-design --from claude` classnames.json
# emission. Combines library-level fixture coverage (no binary
# dependency) with a shell-out against the built CLI when available.
add_executable(pulp-test-cli-import-design test_cli_import_design.cpp)
target_link_libraries(pulp-test-cli-import-design
    PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-cli-import-design PRIVATE
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import-design pulp-cli)
endif()
# The `import-design` command delegates to the standalone pulp-import-design
# helper; without it the revived shell-out cases fail (exit 1) instead of
# exercising the real path, so build it alongside this suite.
if(TARGET pulp-import-design)
    add_dependencies(pulp-test-cli-import-design pulp-import-design)
endif()
catch_discover_tests(pulp-test-cli-import-design
    PROPERTIES
        ENVIRONMENT "PULP_REPO_ROOT=${CMAKE_SOURCE_DIR}"
        LABELS "parser-import")

# `pulp design {lint,diff,compile,lint-adherence}` must reject partial
# DESIGN.md parse results before analyzing or emitting artifacts.
add_executable(pulp-test-cli-designmd-subcommands
    test_cli_designmd_subcommands.cpp)
target_link_libraries(pulp-test-cli-designmd-subcommands
    PRIVATE pulp::platform Catch2::Catch2WithMain)
# The GPU-enabled desktop configure attaches the real CLI binary and build
# dependency after `pulp-cli` is declared in tools/cli/CMakeLists.txt.
catch_discover_tests(pulp-test-cli-designmd-subcommands
    PROPERTIES LABELS "parser-import")

# Direct shell-out coverage for the standalone import-design tool
# executable. These tests hit tools/import-design/pulp_import_design.cpp
# without going through the top-level pulp CLI delegate.
add_executable(pulp-test-import-design-tool test_import_design_tool.cpp
    # Same miniz subset the CLI links — `.pulp.zip` auto-unpack
    # test needs to assemble a tiny ZIP fixture from C++ rather than
    # shelling out to `zip(1)` (which is not portable).
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tdef.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_zip.c
    # Compile the fig lane in-process so its logic is exercised (and covered)
    # directly, not only through the CLI subprocess.
    ${CMAKE_SOURCE_DIR}/tools/import-design/fig_lane.cpp
    # fig_lane's multi-state merge lives here (and is shared with the
    # repeated---file lane), so the in-process fig_lane cases need it linked.
    ${CMAKE_SOURCE_DIR}/tools/import-design/envelope_merge.cpp
    # fig_lane resolves the Node decoder through the shared search.
    ${CMAKE_SOURCE_DIR}/tools/import-design/node_runtime.cpp)
target_include_directories(pulp-test-import-design-tool PRIVATE
    ${CMAKE_SOURCE_DIR}/external/miniz
    ${CMAKE_SOURCE_DIR}/tools/import-design
    # envelope_merge.cpp parses the design envelope with choc::json. This target
    # deliberately does not link pulp::view (which re-exports these headers), so
    # the include path is named directly rather than dragging the pipeline in.
    ${choc_SOURCE_DIR})
target_link_libraries(pulp-test-import-design-tool
    PRIVATE pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-import-design-tool PRIVATE
    PULP_IMPORT_DESIGN_TOOL_PATH="$<TARGET_FILE:pulp-import-design>"
    PULP_FIG_FIXTURE="${CMAKE_SOURCE_DIR}/test/fixtures/imports/fig/synthetic.fig"
    PULP_FIG_DECODE_SCRIPT="${CMAKE_SOURCE_DIR}/tools/import-design/fig_decode.mjs"
    PULP_FIGMA_REST_EXPORT="${CMAKE_SOURCE_DIR}/tools/import-design/figma_rest_export.py")
add_dependencies(pulp-test-import-design-tool pulp-import-design)
if(WIN32)
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import;windows-pr-quarantine")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            RESOURCE_LOCK "design-import-network"
        LABELS "parser-import;windows-pr-quarantine")
else()
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            RESOURCE_LOCK "design-import-network"
        LABELS "parser-import")
endif()

# Offline .fig decoder unit tests (Node). Cover kiwi decode, container
# unpacking, outline, envelope materialization, and vector-geometry lowering
# against a committed synthetic fixture. Skipped when Node is unavailable.
#
# Globbed rather than listed file-by-file: naming one entry point meant a new
# `*.test.mjs` beside it was silently never executed, so a suite could be green
# in CI while testing nothing. CONFIGURE_DEPENDS re-globs when a file is added.
#
# The glob covers the tool directory as well as fig/, for the same reason: the
# import-design tools beside the decoder (material_audit.mjs) are part of this
# suite, and a test file that no glob names is a suite testing nothing.
if(_PULP_NODE_FOR_TESTS)
    file(GLOB _PULP_FIG_TESTS CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/tools/import-design/fig/*.test.mjs
         ${CMAKE_SOURCE_DIR}/tools/import-design/*.test.mjs)
    add_test(NAME pulp-fig-decode-unit
             COMMAND ${_PULP_NODE_FOR_TESTS} --test ${_PULP_FIG_TESTS})
    set_tests_properties(pulp-fig-decode-unit PROPERTIES
        TIMEOUT 60
        LABELS "parser-import;node")
endif()

# `--execute-bundle` harness end-to-end. Boots the
# ScriptEngine + WidgetBridge, builds a synthetic React-like bundle on
# the fly, asserts the materialized DOM walker produces an IR deeper
# than the loader-shell baseline. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (Spectr's editor.html).
add_executable(pulp-test-design-import-claude-runtime test_design_import_claude_runtime.cpp)
target_link_libraries(pulp-test-design-import-claude-runtime
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-claude-runtime
    PROPERTIES LABELS "parser-import")

# Inline `<script>` evaluation in the `--execute-bundle`
# harness. Asserts that inline `text/javascript`, inline `text/babel`
# (via a Babel-standalone shim), DOMContentLoaded dispatch, and the
# layered async drain all fire. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (canonical Spectr
# editor.html — should now produce >20 nodes, not the 11-element
# loader shell previously seen before the full editor materialized).
add_executable(pulp-test-design-import-inline-babel test_design_import_inline_babel.cpp)
target_link_libraries(pulp-test-design-import-inline-babel
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-inline-babel
    PROPERTIES LABELS "parser-import")

# JSX instrument import harness. Loads a
# pre-compiled JSX bundle from PULP_JSX_BUNDLE and asserts that the
# Claude-style runtime harness can materialize it.
add_executable(pulp-test-design-import-jsx-runtime test_design_import_jsx_runtime.cpp)
target_link_libraries(pulp-test-design-import-jsx-runtime
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-jsx-runtime
    PROPERTIES LABELS "parser-import")

# Screenshot comparison tests (visual diff for design import validation)
if(APPLE)
    add_executable(pulp-test-screenshot-compare test_screenshot_compare.cpp)
    target_link_libraries(pulp-test-screenshot-compare PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-compare
        PROPERTIES TIMEOUT 240
        LABELS "parser-import")
endif()

# `pulp import-design --url <figma.com scene URL>` guard. The classifier lives
# in the header-only, dependency-free figma_url.hpp (same rationale as
# import_detect.hpp), so the rule is covered in every lane without linking the
# import pipeline; the shell-out case additionally exercises the real CLI path
# when the binary is built.
add_executable(pulp-test-cli-import-figma-url test_cli_import_figma_url.cpp)
target_include_directories(pulp-test-cli-import-figma-url PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/import-design)
target_link_libraries(pulp-test-cli-import-figma-url
    PRIVATE Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import-figma-url pulp-cli)
endif()
if(TARGET pulp-import-design)
    add_dependencies(pulp-test-cli-import-figma-url pulp-import-design)
endif()
catch_discover_tests(pulp-test-cli-import-figma-url
    PROPERTIES LABELS "parser-import")
