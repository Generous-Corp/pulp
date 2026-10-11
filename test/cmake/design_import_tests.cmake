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
#
# Registered UNCONDITIONALLY, and that is the point. `PULP_DESIGN_PANEL_PACK_CSS`
# defaults to empty and nothing in the repo sets it, so guarding registration on
# it did not skip these tests — it meant they were never registered at all. They
# did not appear in `ctest -N`, and a green run said exactly as much about this
# gate as it would have said if the gate had been deleted. The skip belongs to
# the script, which already exits 77 with a named reason when the pack, the
# importer or the browser is missing; a skip you can SEE is strictly better than
# a silence you cannot.
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
    RESOURCE_LOCK browser
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
    RESOURCE_LOCK browser
    TIMEOUT 600
    SKIP_RETURN_CODE 77)

# Design-unit boundary. pulp-design-ir (value types) sits below view-core;
# pulp-design-import (importers, codegen) and the Pulp-owned audio-control
# recognition pack sit above it. The files share core/view, so the boundary is
# recorded only in the targets' file lists and link edges: configure writes
# those resolved properties here and the gate checks link edges plus direct
# includes. The unit test drives the same gate over synthetic trees with one
# injected violation each, so a gate that cannot see a violation fails there.
set(_pulp_design_unit_targets pulp-design-ir pulp-view-core)
if(TARGET pulp-design-import)
    list(APPEND _pulp_design_unit_targets pulp-design-import pulp-design-import-audio-pack)
endif()
set(_pulp_design_unit_manifest_content "")
foreach(_unit IN LISTS _pulp_design_unit_targets)
    foreach(_field_property IN ITEMS
            "type=TYPE" "source_dir=SOURCE_DIR" "sources=SOURCES"
            "links=LINK_LIBRARIES" "interface_links=INTERFACE_LINK_LIBRARIES"
            "headers=PULP_UNIT_HEADERS")
        string(REPLACE "=" ";" _pair "${_field_property}")
        list(GET _pair 0 _field)
        list(GET _pair 1 _property)
        string(APPEND _pulp_design_unit_manifest_content
            "${_unit}\t${_field}\t$<TARGET_PROPERTY:${_unit},${_property}>\n")
    endforeach()
endforeach()
# `.txt`: a configure-written build-tree input with any other extension reads to
# changed_surface_script_families.py as a build product no target owns, which
# would leave the checker script unmapped.
set(_pulp_design_unit_manifest "${CMAKE_BINARY_DIR}/design-unit-boundary/manifest.txt")
file(GENERATE OUTPUT "${_pulp_design_unit_manifest}"
    CONTENT "${_pulp_design_unit_manifest_content}")
add_test(
    NAME design-unit-boundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/design_unit_boundary_check.py"
        --manifest "${_pulp_design_unit_manifest}")
set_tests_properties(design-unit-boundary PROPERTIES LABELS "parser-import;contract")
add_test(
    NAME design-unit-boundary-selftest
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_design_unit_boundary_check.py")
set_tests_properties(design-unit-boundary-selftest PROPERTIES LABELS "parser-import;contract")
unset(_pulp_design_unit_targets)
unset(_pulp_design_unit_manifest_content)
unset(_pulp_design_unit_manifest)
