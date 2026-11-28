# Included from integrations/pulp-vellum/tests/CMakeLists.txt, which provides
# pulp::vellum, Vellum::MirrorView_core and Catch2 in scope.

# An AU v2 host opening a Vellum editor: the AU v2 contract view test
# source built a second time against Pulp's Vellum bridge, with its own
# class names, running only its Vellum cases.
if(APPLE AND NOT PULP_IOS AND PULP_HAS_AUSDK)
    add_executable(pulp-test-au-v2-vellum-view
        ${PULP_ROOT_DIR}/test/test_au_v2_contract_view.mm
        ${PULP_ROOT_DIR}/core/format/src/au_v2_contract_view.mm)
    target_include_directories(pulp-test-au-v2-vellum-view PRIVATE ${PULP_ROOT_DIR}/test)
    target_link_libraries(pulp-test-au-v2-vellum-view PRIVATE
        pulp::vellum Vellum::MirrorView_core ausdk Catch2::Catch2WithMain
        "-framework AppKit" "-framework AudioToolbox" "-framework AudioUnit")
    target_compile_definitions(pulp-test-au-v2-vellum-view PRIVATE
        PULP_AU_CONTRACT_VIEW_FACTORY_CLASS=PulpAUContractViewFactory_Test
        PULP_AU_CONTRACT_HOST_VIEW_CLASS=PulpAUContractHostView_Test
        PULP_AU_V2_TEST_HOOKS=1 PULP_TEST_AU_VELLUM=1)
    set_target_properties(pulp-test-au-v2-vellum-view PROPERTIES CXX_STANDARD 23)
    target_compile_options(pulp-test-au-v2-vellum-view PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
    target_link_options(pulp-test-au-v2-vellum-view PRIVATE -Wl,-export_dynamic)
    catch_discover_tests(pulp-test-au-v2-vellum-view TEST_SPEC "[vellum]"
        PROPERTIES LABELS pulp-vellum-editor)
endif()
