# On Linux a VST3 plug-in is the bundle the VST3 SDK defines,
# <Name>.vst3/Contents/<machine>-linux/<Name>.so, not a bare shared object: a
# host that follows the SDK's layout finds nothing else. This builds nothing of
# its own; it checks the bundle of an existing editor-less VST3 fixture, and
# that the same check rejects the loose linker output beside it.
if(NOT UNIX OR APPLE OR ANDROID OR NOT PULP_HAS_VST3 OR
   NOT TARGET PulpNoEditorLinkFloor_VST3)
    return()
endif()

set(_vst3_bundle "${CMAKE_BINARY_DIR}/VST3/Pulp No Editor Link Floor.vst3")
add_test(NAME linux-vst3-bundle-layout
    COMMAND "${CMAKE_COMMAND}"
        "-DBUNDLE=${_vst3_bundle}"
        "-DMODULE_NAME=Pulp No Editor Link Floor"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_vst3_bundle.cmake")
set_tests_properties(linux-vst3-bundle-layout PROPERTIES
    LABELS "format;vst3;linux;bundle" TIMEOUT 30)

# Control: the same check on the linker output (a bare module in the staging
# directory) must fail, or it would pass anything.
add_test(NAME linux-vst3-bundle-layout-rejects-bare-module
    COMMAND "${CMAKE_COMMAND}"
        "-DBUNDLE=$<TARGET_FILE:PulpNoEditorLinkFloor_VST3>"
        "-DMODULE_NAME=Pulp No Editor Link Floor"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_vst3_bundle.cmake")
set_tests_properties(linux-vst3-bundle-layout-rejects-bare-module PROPERTIES
    LABELS "format;vst3;linux;bundle" TIMEOUT 30 WILL_FAIL TRUE)
