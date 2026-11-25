# The bridge's tests, added once Catch2 exists (see the root CMakeLists.txt).
# Sources are rooted at PULP_ROOT_DIR, not PROJECT_SOURCE_DIR: when Pulp is
# embedded with add_subdirectory, PROJECT_SOURCE_DIR is the consumer's tree.

find_package(Threads REQUIRED)

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_param_bridge.cpp)
    target_link_libraries(pulp-test-pulp-vellum PRIVATE
        pulp::vellum Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum PROPERTIES LABELS pulp-vellum)

    # The SDK's imported targets can be scoped to the integration directory.
    get_property(_has_swift_motion TARGET pulp-vellum PROPERTY PULP_VELLUM_HAS_SWIFT_MOTION)
    if(_has_swift_motion)
        add_executable(pulp-test-motion-swift-bridge
            test_motion_swift_bridge.cpp
            ${PULP_ROOT_DIR}/apple/Sources/PulpSwift/PulpBridge.cpp)
        target_include_directories(pulp-test-motion-swift-bridge PRIVATE
            ${PULP_ROOT_DIR}/apple/Sources/PulpSwift)
        target_link_libraries(pulp-test-motion-swift-bridge PRIVATE
            pulp::vellum Catch2::Catch2WithMain Threads::Threads)
        catch_discover_tests(pulp-test-motion-swift-bridge PROPERTIES
            LABELS pulp-vellum TIMEOUT 30)
    endif()

    # The editor tests need Vellum's mirrored view layer; skip them for an SDK
    # without it so the parameter-bridge tests still build.
    if(TARGET Vellum::MirrorView_core)
        add_executable(pulp-test-pulp-vellum-editor
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_editor.cpp)
        target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE
            pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain
            Threads::Threads)
        if(TARGET Vellum::MirrorView_script)
            target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE Vellum::MirrorView_script)
            target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                PULP_TEST_VELLUM_SCRIPTED_UI=1)
        endif()
        if(TARGET pulp-vellum-scripted)
            add_executable(pulp-test-vellum-scripted-wiring test_scripted_editor_wiring.cpp)
            target_link_libraries(pulp-test-vellum-scripted-wiring PRIVATE
                pulp-vellum-scripted Vellum::MirrorView_script Catch2::Catch2WithMain Threads::Threads)
            # Keep format-core's environment gate deterministic in CI. This
            # executable records the external GPU/session and scheduler edges.
            add_test(NAME pulp-vellum-scripted-wiring
                COMMAND ${CMAKE_COMMAND} -E env CI=0 PULP_HEADLESS=0 PULP_TEST_MODE=0
                    PULP_DISABLE_PLUGIN_EDITOR=0 PULP_EDITOR_PREWARM=1 DISPLAY=:0
                    $<TARGET_FILE:pulp-test-vellum-scripted-wiring>)
            set_tests_properties(pulp-vellum-scripted-wiring PROPERTIES
                LABELS pulp-vellum-editor TIMEOUT 30)
            target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                PULP_TEST_VELLUM_SCRIPTED_EDITOR=1)
            if(TARGET pulp-vellum-audio)
                target_compile_definitions(pulp-test-vellum-scripted-wiring PRIVATE
                    PULP_VELLUM_AUDIO=1)
                target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                    PULP_TEST_VELLUM_SCRIPTED_AUDIO=1)
            endif()
            _pulp_vellum_scripted_package("scripted editor fixture" _scripted_package)
            target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE "${_scripted_package}")
            # The audio display widgets (meter, spectrum) need the view-audio
            # provider, so without pulp-vellum-audio the script is the knob and
            # label alone; main.js would throw at its first createMeter.
            set(_scripted_fixture main-core.js)
            if(TARGET pulp-vellum-audio)
                set(_scripted_fixture main.js)
            endif()
            _pulp_apply_ui_script_definition(pulp-test-pulp-vellum-editor
                "${PULP_ROOT_DIR}/test/fixtures/scripted-editor/${_scripted_fixture}")
            set_property(TARGET pulp-test-pulp-vellum-editor PROPERTY PULP_VELLUM_SCRIPTED_EDITOR TRUE)
        endif()
        catch_discover_tests(pulp-test-pulp-vellum-editor PROPERTIES LABELS pulp-vellum-editor)
        add_test(NAME pulp-vellum-editor-bootstrap-failure
            COMMAND pulp-test-pulp-vellum-editor "[.bootstrap-failure]")
        set_tests_properties(pulp-vellum-editor-bootstrap-failure PROPERTIES
            LABELS pulp-vellum-editor)

        # A VST3 host opening a Vellum editor through the view-free contract
        # view. The cases define make_plug_view() themselves, so the
        # create_view() path's view layer is never linked.
        if(PULP_HAS_VST3)
            target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                PULP_TEST_VELLUM_VST3=1)
        endif()

        add_executable(pulp-test-pulp-vellum-about-notices
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_about_notices.cpp)
        target_link_libraries(pulp-test-pulp-vellum-about-notices PRIVATE
            pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain Threads::Threads)
        catch_discover_tests(pulp-test-pulp-vellum-about-notices PROPERTIES LABELS pulp-vellum-editor)

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

        # An AU v3 host opening a Vellum editor: the AU v3 contract view test
        # source built a second time against Pulp's Vellum bridge, running only
        # its Vellum cases.
        if(APPLE AND NOT PULP_IOS)
            add_executable(pulp-test-au-v3-vellum-view
                ${PULP_ROOT_DIR}/test/test_au_v3_contract_view.mm
                ${PULP_ROOT_DIR}/core/format/src/au_v3_contract_view.mm)
            target_include_directories(pulp-test-au-v3-vellum-view PRIVATE ${PULP_ROOT_DIR}/test)
            target_link_libraries(pulp-test-au-v3-vellum-view PRIVATE
                pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain "-framework AppKit")
            target_compile_definitions(pulp-test-au-v3-vellum-view PRIVATE
                PULP_AUV3_CONTRACT_VIEW_CLASS=PulpAUv3ContractView_Test PULP_TEST_AU_VELLUM=1)
            set_target_properties(pulp-test-au-v3-vellum-view PROPERTIES CXX_STANDARD 23)
            target_compile_options(pulp-test-au-v3-vellum-view PRIVATE
                $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
            catch_discover_tests(pulp-test-au-v3-vellum-view TEST_SPEC "[vellum]"
                PROPERTIES LABELS pulp-vellum-editor)
        endif()

        # The standalone app's contract window hosting a Vellum editor: its
        # test source built a second time against Pulp's Vellum bridge and the
        # view-free standalone core, running only its Vellum cases.
        if(APPLE AND NOT PULP_IOS AND TARGET pulp-standalone-contract)
            add_executable(pulp-test-standalone-vellum-editor
                ${PULP_ROOT_DIR}/test/test_standalone_contract_editor.mm)
            target_include_directories(pulp-test-standalone-vellum-editor PRIVATE
                ${PULP_ROOT_DIR}/test)
            target_link_libraries(pulp-test-standalone-vellum-editor PRIVATE
                pulp::vellum pulp::standalone-contract Vellum::MirrorView_core
                Catch2::Catch2WithMain "-framework AppKit")
            target_compile_definitions(pulp-test-standalone-vellum-editor PRIVATE
                PULP_TEST_STANDALONE_VELLUM=1)
            catch_discover_tests(pulp-test-standalone-vellum-editor TEST_SPEC "[vellum]"
                PROPERTIES LABELS pulp-vellum-editor)
        endif()
    else()
        message(STATUS "pulp-vellum: Vellum::MirrorView_core not in this Vellum SDK; "
            "skipping pulp-test-pulp-vellum-editor")
    endif()
endif()
