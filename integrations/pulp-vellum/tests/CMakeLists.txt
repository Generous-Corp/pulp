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

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET Vellum::MirrorView_script)
        add_executable(pulp-test-pulp-vellum-prewarm
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_editor_prewarm.cpp)
        target_link_libraries(pulp-test-pulp-vellum-prewarm PRIVATE
            pulp::vellum Vellum::MirrorView_script Catch2::Catch2WithMain)
        add_test(NAME pulp.vellum-editor-prewarm COMMAND pulp-test-pulp-vellum-prewarm)
        set_tests_properties(pulp.vellum-editor-prewarm PROPERTIES
            LABELS pulp-vellum-editor TIMEOUT 30
            ENVIRONMENT "CI=0;PULP_HEADLESS=0;PULP_TEST_MODE=0;PULP_DISABLE_PLUGIN_EDITOR=0;PULP_EDITOR_PREWARM=1")
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
            _pulp_vellum_scripted_package("scripted editor fixture" _scripted_package)
            target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE "${_scripted_package}")
            _pulp_apply_ui_script_definition(pulp-test-pulp-vellum-editor
                "${PULP_ROOT_DIR}/test/fixtures/scripted-editor/main.js")
            set_property(TARGET pulp-test-pulp-vellum-editor PROPERTY PULP_VELLUM_SCRIPTED_EDITOR TRUE)
        endif()
        catch_discover_tests(pulp-test-pulp-vellum-editor PROPERTIES LABELS pulp-vellum-editor)

        add_executable(pulp-test-pulp-vellum-about-notices
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_about_notices.cpp)
        target_link_libraries(pulp-test-pulp-vellum-about-notices PRIVATE
            pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain Threads::Threads)
        catch_discover_tests(pulp-test-pulp-vellum-about-notices PROPERTIES LABELS pulp-vellum-editor)
    else()
        message(STATUS "pulp-vellum: Vellum::MirrorView_core not in this Vellum SDK; "
            "skipping pulp-test-pulp-vellum-editor")
    endif()
endif()
