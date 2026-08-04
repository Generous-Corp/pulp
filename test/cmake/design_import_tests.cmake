# Design import test registrations.
# Included by test/CMakeLists.txt; add focused registrations to the included manifests.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_view_widget_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_native_codegen_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_runtime_bridge_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_tool_cli_tests.cmake")

# The native design path's hard-won invariants, asserted on a real generated
# panel rather than a reduced fixture. 100% native is the load-bearing one: a
# panel can look correct while being a screenshot of Chrome with live controls
# layered over it, and only `lowered == native` with zero capture fallback
# rules that out.
#
# Needs a design pack — the fixture links `styles.css` and the pack is not
# Pulp's to ship — so it skips (77) with a named reason rather than passing
# over an unstyled import.
if(PULP_DESIGN_PANEL_PACK_CSS)
    add_test(
        NAME agent-panel-native-invariants
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/import-validation/check_agent_panel_invariants.py"
            --repo "${CMAKE_SOURCE_DIR}"
            --panel "${CMAKE_SOURCE_DIR}/test/fixtures/agent-panels/magneto"
            --pack-css "${PULP_DESIGN_PANEL_PACK_CSS}"
            --pack-fonts "${PULP_DESIGN_PANEL_PACK_FONTS}")
    set_tests_properties(agent-panel-native-invariants PROPERTIES
        LABELS "parser-import;browser-capture"
        TIMEOUT 600
        SKIP_RETURN_CODE 77)

    # The negative fixture. polystrike declares a root shorter than its own
    # content, so CHROME clips four controls too — the oracle itself is broken
    # and there is nothing to compare against. Keeping it is what exercises
    # `capture-control-clipped` end to end; without it that gate has no test,
    # and a gate nothing tests is a gate that can stop firing silently.
    #
    # It passes by being REFUSED. A fixture that fails on purpose is not a
    # failing suite.
    add_test(
        NAME agent-panel-clipped-is-rejected
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/import-validation/check_agent_panel_invariants.py"
            --repo "${CMAKE_SOURCE_DIR}"
            --panel "${CMAKE_SOURCE_DIR}/test/fixtures/agent-panels/polystrike"
            --pack-css "${PULP_DESIGN_PANEL_PACK_CSS}"
            --pack-fonts "${PULP_DESIGN_PANEL_PACK_FONTS}"
            --expect-reject "capture-control-clipped")
    set_tests_properties(agent-panel-clipped-is-rejected PROPERTIES
        LABELS "parser-import;browser-capture"
        TIMEOUT 600
        SKIP_RETURN_CODE 77)
endif()
