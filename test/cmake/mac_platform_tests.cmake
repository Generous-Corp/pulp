# macOS platform harness and key-equivalent tests.
# Included by test/CMakeLists.txt; keep related test registrations here.
# ── Mac platform-test harness ────────────────────────────────
#
# Catch2 fixture that brings up a hidden NSWindow + CAMetalLayer host
# WITHOUT calling makeKeyAndOrderFront / activateIgnoringOtherApps, so
# tests can exercise window_host_mac.mm code paths (set_design_viewport,
# paint_scene, mouse-coordinate inverse mapping, resize → contentsScale)
# that today are structurally untestable from plain Catch2.
#
# Covers the hidden window / capture_back_buffer_png contract plus
# synthetic mouse, wheel, and context-menu consumers.
# The target is macOS+Skia-only because it drives real NSWindow and
# CAMetalLayer objects rather than a mock platform backend.
if(APPLE AND NOT PULP_IOS AND PULP_HAS_SKIA)
    add_executable(pulp-test-mac-platform-harness
        test_mac_platform_harness.cpp
        mac_window_harness.mm
    )
    target_link_libraries(pulp-test-mac-platform-harness PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
        "-framework Metal"
        "-framework QuartzCore"
    )
    target_include_directories(pulp-test-mac-platform-harness PRIVATE
        ${CMAKE_SOURCE_DIR}/external/miniz)
    target_compile_definitions(pulp-test-mac-platform-harness PRIVATE
        PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    catch_discover_tests(pulp-test-mac-platform-harness)
endif()
if(APPLE AND NOT PULP_IOS)
    # Pin the invariant -[PulpView liveFocusedView] depends on:
    # ~View() must auto-clear focused_input_ so the
    # accessor can safely re-sync the ivar to nullptr before any deref.
    add_executable(pulp-test-mac-mousedown-stale-focus
        test_mac_mousedown_stale_focus.mm
    )
    target_link_libraries(pulp-test-mac-mousedown-stale-focus PRIVATE
        pulp::view
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-mac-mousedown-stale-focus)
endif()
if(APPLE AND NOT PULP_IOS)
    # Pin macOS performKeyEquivalent routing, text-input
    # protocol conformance, and TextEditor-specific command priority.
    add_executable(pulp-test-mac-perform-key-equivalent
        test_mac_perform_key_equivalent.mm
    )
    target_link_libraries(pulp-test-mac-perform-key-equivalent PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
    )
    # Pull the PulpView archive member without force-loading unrelated view-core objects.
    target_link_options(pulp-test-mac-perform-key-equivalent PRIVATE
        "LINKER:-u,_OBJC_CLASS_$_PulpView"
    )
    catch_discover_tests(pulp-test-mac-perform-key-equivalent)
endif()
