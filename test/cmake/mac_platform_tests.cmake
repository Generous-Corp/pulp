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
        pulp::view-script
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

# Grouped executable (pulp_add_test_group in tools/cmake/PulpTestSuite.cmake):
# the AppKit suites that link only pulp::view share one binary; each keeps its
# own registration and properties. The group links the union of their
# frameworks and pulls the PulpView / PulpPluginView archive members with -u,
# because several cases message those classes without referencing a C++
# symbol from window_host_mac.mm. A suite stays on its own when it needs its
# own process or compile line: the Skia-only platform harness (pulp::view-script
# plus the miniz include root), foreign-framework coexistence (pulp::events;
# it measures a process shared with one foreign window), the CoreMIDI
# client (pulp::midi; one client per process is the invariant), and the live
# hover-cursor suite, which drives the real system pointer.
if(APPLE AND NOT PULP_IOS)
    pulp_add_test_group(pulp-test-group-mac-view LIBRARIES pulp::view)
    target_link_libraries(pulp-test-group-mac-view PRIVATE
        "-framework AppKit"
        "-framework CoreVideo"
        "-framework QuartzCore")
    target_link_options(pulp-test-group-mac-view PRIVATE
        "LINKER:-u,_OBJC_CLASS_$_PulpView"
        "LINKER:-u,_OBJC_CLASS_$_PulpPluginView")
endif()
if(APPLE AND NOT PULP_IOS)
    # Frame-timing seam of the CVDisplayLink-driven macOS hosts: the nominal
    # (first-frame / wake) interval seed, whether the CPU plugin-view host runs a
    # render link for a NATIVE (non-scripted) editor, what that link costs while
    # the editor is static (it runs inside a DAW), and that PulpView's teardown
    # drops its pointers into the freed host. All four answers come from
    # CoreVideo/AppKit, so they cannot be pinned from a portable C++ test.
    pulp_add_test_suite(pulp-test-mac-frame-timing GROUP pulp-test-group-mac-view
        SOURCES test_mac_frame_timing.mm
        LIBRARIES pulp::view)
    # CVDisplayLink is deprecated in macOS 15 but is still the only vsync source
    # the hosts use (see window_host_mac.mm); the header under test calls it.
    set_source_files_properties(test_mac_frame_timing.mm PROPERTIES
        COMPILE_OPTIONS -Wno-deprecated-declarations)
endif()
if(APPLE AND NOT PULP_IOS)
    # Pin the invariant -[PulpView liveFocusedView] depends on:
    # ~View() must auto-clear focused_input_ so the
    # accessor can safely re-sync the ivar to nullptr before any deref.
    pulp_add_test_suite(pulp-test-mac-mousedown-stale-focus GROUP pulp-test-group-mac-view
        SOURCES test_mac_mousedown_stale_focus.mm
        LIBRARIES pulp::view)
endif()
if(APPLE AND NOT PULP_IOS)
    # The runtime half of opening a document: the NSApplication delegate that
    # receives application:openURLs:. It drives the real installed delegate,
    # because nothing in a unit test can produce the Apple Event the OS sends.
    # The declaration half (CFBundleDocumentTypes) is covered by
    # test_standalone_document_types.cmake; neither half works alone.
    pulp_add_test_suite(pulp-test-mac-open-document GROUP pulp-test-group-mac-view
        SOURCES test_mac_open_document.mm
        LIBRARIES pulp::view)
endif()
if(APPLE AND NOT PULP_IOS)
    # Pin macOS performKeyEquivalent routing, text-input
    # protocol conformance, and TextEditor-specific command priority.
    pulp_add_test_suite(pulp-test-mac-perform-key-equivalent GROUP pulp-test-group-mac-view
        SOURCES test_mac_perform_key_equivalent.mm
        LIBRARIES pulp::view)
endif()
if(APPLE AND NOT PULP_IOS)
    # Foreign-framework coexistence (PAM WS-6 / G8): a raw non-Pulp NSWindow
    # stands in for any other framework's editor window sharing the process.
    # Pins that the CPU
    # window host's idle pump keeps firing under a modal / event-tracking
    # run-loop mode (the NSRunLoopCommonModes fix in window_host_mac.mm) and
    # that Pulp's own-thread timer, audio-render thread, and UI hit-testing
    # stay correct across the foreign window's open/resize/focus/close.
    add_executable(pulp-test-foreign-framework-coexistence
        test_foreign_framework_coexistence.mm
    )
    target_link_libraries(pulp-test-foreign-framework-coexistence PRIVATE
        pulp::view
        pulp::events
        Catch2::Catch2WithMain
        "-framework AppKit"
    )
    catch_discover_tests(pulp-test-foreign-framework-coexistence)

    # One CoreMIDI client per process, not one per open port. Drives the real
    # CoreMIDI framework because that registration is what the invariant is
    # about; needs no MIDI hardware and opens no ports.
    add_executable(pulp-test-coremidi-shared-client
        test_coremidi_shared_client.cpp
    )
    target_link_libraries(pulp-test-coremidi-shared-client PRIVATE
        pulp::midi
        Catch2::Catch2WithMain
        "-framework CoreMIDI"
    )
    catch_discover_tests(pulp-test-coremidi-shared-client)
endif()
if(APPLE AND NOT PULP_IOS)
    # AppKit re-asks which cursor to show when the pointer MOVES and never
    # because the content under a still pointer changed. These cases drive the
    # hosts' frame-path refresh directly, with no synthesized mouse move or
    # click, and assert the cursor AppKit is told to display follows the region
    # that slid under the pointer. Nothing portable can pin it: the answer lives
    # in +[NSCursor currentCursor].
    pulp_add_test_suite(pulp-test-mac-hover-cursor-stationary GROUP pulp-test-group-mac-view
        SOURCES test_mac_hover_cursor_stationary.mm
        LIBRARIES pulp::view)
endif()
if(APPLE AND NOT PULP_IOS)
    # PULP_TEST_POINTER_DRAG spelling contract. The drive it feeds needs a real
    # NSWindow, hit-test, and display link, so the parse is the only part a
    # portable test can pin — and it is where a typo turns an unattended
    # measurement into a silently idle window. Links pulp::view because the
    # parser is defined beside its consumer in window_host_mac.mm.
    pulp_add_test_suite(pulp-test-mac-test-pointer-drag GROUP pulp-test-group-mac-view
        SOURCES test_mac_test_pointer_drag.cpp
        LIBRARIES pulp::view)
endif()

if(APPLE AND NOT PULP_IOS)
    # Whether a person SEES the cursor change while hovering. The stationary
    # cases above drive the frame-path refresh on a windowless view and assert
    # what the resolver computed; AppKit runs its own cursor pass and can reset
    # that answer before it reaches the screen, so those cases pass whether or
    # not hover works on a real window. These put the shipping view class in a
    # live NSWindow, move the pointer with real CGEvents and no button held,
    # pump the run loop, and read the cursor AppKit settled on.
    #
    # Requires a window-server session, Accessibility trust for synthetic
    # pointer events, and an IDLE machine -- it drives the real pointer, so a
    # human using the mouse corrupts the reading. Each case skips loudly when
    # the session cannot support it rather than reporting a pass it did not
    # measure. Labelled "validation" so it stays off the required headless gate,
    # and "human-input" to mark that it takes over the pointer.
    add_executable(pulp-test-mac-hover-cursor-live
        test_mac_hover_cursor_live.mm
    )
    target_link_libraries(pulp-test-mac-hover-cursor-live PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
        "-framework ApplicationServices"
    )
    # Pull the host archive member; the cases message the class but reference
    # no C++ symbol from the .mm.
    target_link_options(pulp-test-mac-hover-cursor-live PRIVATE
        "LINKER:-u,_OBJC_CLASS_$_PulpView"
    )
    catch_discover_tests(pulp-test-mac-hover-cursor-live
        # RUN_SERIAL must precede LABELS: LABELS is a list property and would
        # otherwise swallow "RUN_SERIAL;TRUE" as two more labels, letting these
        # cases run concurrently and fight over the one system pointer.
        PROPERTIES RUN_SERIAL TRUE
                   LABELS "validation;mac;cursor;human-input")
endif()
if(APPLE AND NOT PULP_IOS)
    # The cursor a window host applies on a BUTTONLESS hover, when the region
    # under the pointer decides its cursor in a pointer-move handler — the
    # shape a scripted UI has. A region with a statically assigned cursor is
    # resolvable by hit-test alone and cannot see this defect, so the scene
    # carries one only as the positive control. Nothing portable can pin the
    # result: the answer lives in +[NSCursor currentCursor].
    pulp_add_test_suite(pulp-test-mac-hover-cursor-delivery GROUP pulp-test-group-mac-view
        SOURCES test_mac_hover_cursor_delivery.mm
        LIBRARIES pulp::view)
endif()

if(APPLE AND NOT PULP_IOS)
    # A native child NSView attached via attach_native_child_view (a WKWebView,
    # a hosted editor) is not in the Pulp View tree and owns its own cursor,
    # but a macOS tracking area is not occluded by subviews — so the host still
    # gets -mouseMoved: over it. These cases pin that a button-less move there
    # leaves the child's cursor alone, with a control move over Pulp-owned
    # content that must still publish the tree's cursor.
    pulp_add_test_suite(pulp-test-mac-native-child-cursor GROUP pulp-test-group-mac-view
        SOURCES test_mac_native_child_cursor.mm
        LIBRARIES pulp::view)
endif()
