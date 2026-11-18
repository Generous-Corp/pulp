# integrations/pulp-native GlSurface tests that run off macOS. The macOS
# offscreen-backend test lives with the other pulp-native tests in
# mac_platform_tests.cmake.

if(WIN32 AND TARGET pulp-native)
    # The WGL backend: child-window lifecycle under a host window, the
    # per-binary window class, client-thread context creation and request
    # selection on whatever driver the machine has, and two-phase teardown.
    add_executable(pulp-test-native-gl-surface-win32 test_native_gl_surface_win32.cpp)
    target_link_libraries(pulp-test-native-gl-surface-win32 PRIVATE
        pulp::native Catch2::Catch2WithMain opengl32 user32 gdi32)
    catch_discover_tests(pulp-test-native-gl-surface-win32)
endif()

if(TARGET pulp-native)
    # The X11 translation tables are pure, so every platform runs them.
    add_executable(pulp-test-native-x11-translate test_native_x11_translate.cpp)
    target_link_libraries(pulp-test-native-x11-translate PRIVATE
        pulp::native Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-native-x11-translate)

    # The Win32 translation tables are pure too.
    add_executable(pulp-test-native-win32-translate test_native_win32_translate.cpp)
    target_link_libraries(pulp-test-native-win32-translate PRIVATE
        pulp::native Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-native-win32-translate)
endif()

if(NOT APPLE AND TARGET pulp-native)
    # The shared part of the package on every non-Apple platform: key names,
    # key routing, GL request selection and the AppKit translation's
    # "nothing translated" answers.
    add_executable(pulp-test-native-input-core test_native_input_core.cpp)
    target_link_libraries(pulp-test-native-input-core PRIVATE
        pulp::native Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-native-input-core)
endif()

if(UNIX AND NOT APPLE AND TARGET pulp-native AND PULP_NATIVE_HAS_X11)
    # The GLX backend: child-window lifecycle and resize following under a host
    # window, X error containment, client-thread context creation on a software
    # renderer, two-phase teardown, and input from synthesized X events and the
    # X test extension. Runs under xvfb-run; a lane without it skips the cases
    # unless the display is required.
    add_executable(pulp-test-native-gl-surface-x11 test_native_gl_surface_x11.cpp)
    target_link_libraries(pulp-test-native-gl-surface-x11 PRIVATE
        pulp::native Catch2::Catch2WithMain X11 GL)
    find_library(PULP_X11_XTEST_LIBRARY Xtst)
    find_path(PULP_X11_XTEST_INCLUDE_DIR X11/extensions/XTest.h)
    if(PULP_X11_XTEST_LIBRARY AND PULP_X11_XTEST_INCLUDE_DIR)
        target_link_libraries(pulp-test-native-gl-surface-x11 PRIVATE ${PULP_X11_XTEST_LIBRARY})
        target_include_directories(pulp-test-native-gl-surface-x11 PRIVATE
            ${PULP_X11_XTEST_INCLUDE_DIR})
        target_compile_definitions(pulp-test-native-gl-surface-x11 PRIVATE PULP_TEST_HAS_XTEST=1)
    endif()
    find_library(PULP_X11_XFIXES_LIBRARY Xfixes)
    find_path(PULP_X11_XFIXES_INCLUDE_DIR X11/extensions/Xfixes.h)
    if(PULP_X11_XFIXES_LIBRARY AND PULP_X11_XFIXES_INCLUDE_DIR)
        target_link_libraries(pulp-test-native-gl-surface-x11 PRIVATE ${PULP_X11_XFIXES_LIBRARY})
        target_include_directories(pulp-test-native-gl-surface-x11 PRIVATE
            ${PULP_X11_XFIXES_INCLUDE_DIR})
        target_compile_definitions(pulp-test-native-gl-surface-x11 PRIVATE PULP_TEST_HAS_XFIXES=1)
    endif()
    set(_pulp_x11_test_env "")
    find_program(PULP_XVFB_RUN xvfb-run)
    if(PULP_XVFB_RUN)
        set_property(TARGET pulp-test-native-gl-surface-x11 PROPERTY
            CROSSCOMPILING_EMULATOR "${PULP_XVFB_RUN};-a")
        # With a virtual server available a missing display or context fails
        # the cases instead of skipping them.
        set(_pulp_x11_test_env ENVIRONMENT "PULP_TEST_REQUIRE_X11_DISPLAY=1")
    endif()
    # xvfb-run -a picks a free display number but races when several start at
    # once, hence the lock.
    # Every property here takes one value, and LABELS comes last: Catch2 folds
    # whatever follows LABELS into the labels.
    catch_discover_tests(pulp-test-native-gl-surface-x11
        PROPERTIES ${_pulp_x11_test_env} TIMEOUT 120 RESOURCE_LOCK pulp_xvfb LABELS native-surface)
endif()
