# Design import tool and CLI test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

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
    ${CMAKE_SOURCE_DIR}/tools/import-design/fig_lane.cpp)
target_include_directories(pulp-test-import-design-tool PRIVATE
    ${CMAKE_SOURCE_DIR}/external/miniz
    ${CMAKE_SOURCE_DIR}/tools/import-design)
target_link_libraries(pulp-test-import-design-tool
    PRIVATE pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-import-design-tool PRIVATE
    PULP_IMPORT_DESIGN_TOOL_PATH="$<TARGET_FILE:pulp-import-design>"
    PULP_FIG_FIXTURE="${CMAKE_SOURCE_DIR}/test/fixtures/imports/fig/synthetic.fig"
    PULP_FIG_DECODE_SCRIPT="${CMAKE_SOURCE_DIR}/tools/import-design/fig_decode.mjs")
add_dependencies(pulp-test-import-design-tool pulp-import-design)
if(WIN32)
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import;windows-pr-quarantine")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            LABELS "parser-import;windows-pr-quarantine"
            RESOURCE_LOCK "design-import-network")
else()
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            LABELS "parser-import"
            RESOURCE_LOCK "design-import-network")
endif()

# Offline .fig decoder unit tests (Node). Cover kiwi decode, container
# unpacking, outline, and envelope materialization against a committed
# synthetic fixture. Skipped when Node is unavailable.
if(_PULP_NODE_FOR_TESTS)
    add_test(NAME pulp-fig-decode-unit
             COMMAND ${_PULP_NODE_FOR_TESTS} --test
                     ${CMAKE_SOURCE_DIR}/tools/import-design/fig/fig.test.mjs)
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
        PROPERTIES LABELS "parser-import" TIMEOUT 240)
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
