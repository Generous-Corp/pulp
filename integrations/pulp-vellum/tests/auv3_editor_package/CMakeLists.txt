# A macOS AUv3 plug-in with a Vellum editor package. The editor view's
# lifecycle hooks live in a static library the plug-in's audio code links, so
# the AUv3 framework holds them and the appex stub does not. The appex must
# link without the generated editor-package source, and the checker reads the
# built images' symbol tables to prove where the editor code ended up.

add_library(pulp-test-auv3-editor-view STATIC editor_view.cpp)
target_link_libraries(pulp-test-auv3-editor-view PUBLIC pulp::vellum Vellum::MirrorView_core)
target_include_directories(pulp-test-auv3-editor-view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(pulp-test-auv3-editor-view PROPERTIES POSITION_INDEPENDENT_CODE ON)

pulp_add_vellum_editor_package(pulp-test-auv3-editor-package
    VIEW_HEADER editor_view.hpp
    VIEW_TYPE ::pulp::test_fixtures::Auv3EditorView
    VIEW_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

# The processor and the AUv3 entry are PulpGain's: pulp_add_plugin reads
# au_v3_entry.cpp from this directory, where it is a link to that example's
# entry, which includes pulp_gain.hpp from the include path below.
pulp_add_plugin(PulpAuv3VellumEditor
    UI                pulp-test-auv3-editor-package
    FORMATS           AUv3
    PLUGIN_NAME       "Pulp AUv3 Vellum Editor"
    BUNDLE_ID         "dev.pulp.test.auv3-vellum-editor"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PAvE"
    MANUFACTURER_CODE "PlEu")
target_link_libraries(PulpAuv3VellumEditor_Core INTERFACE pulp-test-auv3-editor-view)
target_include_directories(PulpAuv3VellumEditor_Core INTERFACE
    ${PULP_ROOT_DIR}/examples/pulp-gain)

if(CMAKE_NM AND TARGET PulpAuv3VellumEditor_AUv3 AND TARGET PulpAuv3VellumEditor_AUv3Framework)
    add_test(NAME vellum-auv3-appex-editor-package
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DFRAMEWORK=$<TARGET_FILE:PulpAuv3VellumEditor_AUv3Framework>
            -DAPPEX=$<TARGET_FILE:PulpAuv3VellumEditor_AUv3>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_appex_editor_package.cmake)
    set_tests_properties(vellum-auv3-appex-editor-package PROPERTIES
        LABELS pulp-vellum TIMEOUT 60)
endif()
