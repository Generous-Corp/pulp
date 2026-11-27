# The bridge's tests, added once Catch2 exists (see the root CMakeLists.txt).
# Sources are rooted at PULP_ROOT_DIR, not PROJECT_SOURCE_DIR: when Pulp is
# embedded with add_subdirectory, PROJECT_SOURCE_DIR is the consumer's tree.

find_package(Threads REQUIRED)

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    # test_reload_capabilities_vellum.cpp checks Pulp's reload capability names
    # against Vellum's and runs the remote UX update gate with no Pulp view.
    add_executable(pulp-test-pulp-vellum
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_param_bridge.cpp
        test_reload_capabilities_vellum.cpp)
    target_link_libraries(pulp-test-pulp-vellum PRIVATE
        pulp::vellum Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum PROPERTIES LABELS pulp-vellum)

    # The adapter probe against the real GPU runtime: it must pass, and with the
    # adapter name dropped it must fail for that reason.
    if(TARGET pulp-vellum-gpu-adapter-probe)
        add_test(NAME pulp-vellum-gpu-adapter-probe
            COMMAND pulp-vellum-gpu-adapter-probe)
        set_tests_properties(pulp-vellum-gpu-adapter-probe PROPERTIES
            LABELS pulp-vellum TIMEOUT 60
            ENVIRONMENT "MTL_DEBUG_LAYER=0"
            PASS_REGULAR_EXPRESSION "\"verdict\":\"pass\".*\"swatches_mismatched\":0")
        add_test(NAME pulp-vellum-gpu-adapter-probe-empty-adapter
            COMMAND pulp-vellum-gpu-adapter-probe --simulate-empty-adapter)
        set_tests_properties(pulp-vellum-gpu-adapter-probe-empty-adapter PROPERTIES
            LABELS pulp-vellum TIMEOUT 60
            ENVIRONMENT "MTL_DEBUG_LAYER=0"
            PASS_REGULAR_EXPRESSION "\"verdict\":\"fail\",\"reason\":\"empty-adapter\"")
    endif()

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
        # Header weight: each public header preprocesses on its own within
        # budget and pulls in none of the implementation headers it keeps out.
        include("${PULP_ROOT_DIR}/test/support/PulpHeaderWeight.cmake")
        pulp_add_header_weight_test(pulp-vellum-header-weight-view-editor
            HEADER pulp/vellum/view_editor.hpp USAGE pulp-vellum
            FORBID "pulp/vellum/editor\\.hpp|/vellum/view/view\\.hpp"
            MAX_LINES 80000 LABELS "pulp-vellum;header-weight")
        # A CLAP host opening a Vellum editor through the view-free contract
        # GUI: the plug-in entry compiles without PULP_CLAP_GUI and links the
        # view-free CLAP adapter, as a `UI <package>` plug-in does.
        if(TARGET pulp-format-clap-core)
            target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE
                pulp::format-clap-core clap)
            target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                PULP_TEST_VELLUM_CLAP=1)
        endif()
        if(TARGET Vellum::MirrorView_script)
            target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE Vellum::MirrorView_script)
            target_compile_definitions(pulp-test-pulp-vellum-editor PRIVATE
                PULP_TEST_VELLUM_SCRIPTED_UI=1)
            # The live-swap UI stage over a real Vellum scripted session.
            target_sources(pulp-test-pulp-vellum-editor PRIVATE test_scripted_ui_swap_unit.cpp)
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
            # The view-free reload shell under the Vellum scripted editor. It
            # links format-core and Vellum only, so a Pulp view include in the
            # shell fails this build, and it hot-swaps the logic fixtures that
            # test/cmake/format_reload_tests.cmake builds.
            if(TARGET pulp-reload-logic-unity AND NOT WIN32)
                add_executable(pulp-test-vellum-reload-shell test_reloadable_shell_editor.cpp)
                target_link_libraries(pulp-test-vellum-reload-shell PRIVATE
                    pulp-vellum-scripted Vellum::MirrorView_script Catch2::Catch2WithMain
                    Threads::Threads ${CMAKE_DL_LIBS})
                add_dependencies(pulp-test-vellum-reload-shell
                    pulp-reload-logic-unity pulp-reload-logic-compat pulp-reload-logic-incompat)
                target_compile_definitions(pulp-test-vellum-reload-shell PRIVATE
                    RELOAD_LOGIC_UNITY="$<TARGET_FILE:pulp-reload-logic-unity>"
                    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
                    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
                    # In the build tree: macOS dyld refuses an unsigned image
                    # from a world-writable temp directory.
                    RELOAD_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
                # The real scripted editor, opened through a host-driven editor
                # session, reads the knob-and-label fixture script.
                _pulp_vellum_scripted_package("reload shell editor fixture" _reload_package)
                target_link_libraries(pulp-test-vellum-reload-shell PRIVATE "${_reload_package}")
                _pulp_apply_ui_script_definition(pulp-test-vellum-reload-shell
                    "${PULP_ROOT_DIR}/test/fixtures/scripted-editor/main-core.js")
                set_property(TARGET pulp-test-vellum-reload-shell
                    PROPERTY PULP_VELLUM_SCRIPTED_EDITOR TRUE)
                catch_discover_tests(pulp-test-vellum-reload-shell PROPERTIES
                    LABELS pulp-vellum-editor TIMEOUT 60)
            endif()
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
            # The capture case decodes the host's back buffer with ImageIO.
            if(APPLE)
                target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE
                    "-framework CoreGraphics" "-framework ImageIO")
            endif()
        endif()

        add_executable(pulp-test-pulp-vellum-about-notices
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_about_notices.cpp)
        target_link_libraries(pulp-test-pulp-vellum-about-notices PRIVATE
            pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain Threads::Threads)
        catch_discover_tests(pulp-test-pulp-vellum-about-notices PROPERTIES LABELS pulp-vellum-editor)
        # A macOS AUv3 plug-in with a Vellum editor package: the appex stub
        # must link, and the editor code must live in the framework.
        if(APPLE AND NOT PULP_IOS)
            add_subdirectory(auv3_editor_package)
        endif()


        # An AU v2 host opening a Vellum editor. The target defines a test-only
        # macro, so its definition lives under test/ with the other test targets.
        include(${PULP_ROOT_DIR}/test/cmake/au_v2_vellum_view_tests.cmake)
    else()
        message(STATUS "pulp-vellum: Vellum::MirrorView_core not in this Vellum SDK; "
            "skipping pulp-test-pulp-vellum-editor")
    endif()
endif()
