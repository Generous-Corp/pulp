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
