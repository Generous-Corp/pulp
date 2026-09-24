# Motion, provenance, bridge, and visual-analysis tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Grouped executables for this manifest (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake): each member keeps its own registration
# and properties; only the binary behind them is shared. The pulp::view suites
# share one executable and the pulp::inspect suites another (their compile
# line adds the inspect include root). A suite stays on its own when it needs
# its own process or compile line: meter-source carries the RT allocation
# probe, and the Swift and Android bridge suites compile a bridge .cpp into
# the test.
set(_pulp_motion_group_libs pulp::view)
if(TARGET pulp::render)
    list(APPEND _pulp_motion_group_libs pulp::render)
endif()
pulp_add_test_group(pulp-test-group-motion LIBRARIES ${_pulp_motion_group_libs})

# Animation tests
pulp_add_test_suite(pulp-test-animation GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# FrameClock tests
pulp_add_test_suite(pulp-test-frame-clock GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Host frame-timing seam: measured-dt pump (60/120 Hz, variable refresh,
# dropped + coalesced callbacks, first frame, wake-from-idle) and the
# one-dt-to-every-consumer contract the hosts are wired to.
pulp_add_test_suite(pulp-test-host-frame-pump GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Host->view value sources, subscription lifecycle, and transport-free telemetry
# sidecars. Carries the allocation probe used to assert paint and producer paths.
pulp_add_test_suite(pulp-test-meter-source
    SOURCES test_meter_source.cpp test_value_channel_telemetry.cpp
            harness/rt_allocation_probe.cpp
    LIBRARIES pulp::view)

# Motion bedrock tests.
pulp_add_test_suite(pulp-test-motion GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# MotionPreferences reduced-motion policy + animation honoring.
pulp_add_test_suite(pulp-test-motion-preferences GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Motion inspector protocol-domain tests.
if(TARGET pulp::inspect)
    pulp_add_test_group(pulp-test-group-motion-inspect LIBRARIES pulp::view pulp::inspect)

    pulp_add_test_suite(pulp-test-motion-inspector GROUP pulp-test-group-motion-inspect
        LIBRARIES pulp::view pulp::inspect)

    # Motion scrubber consumes .motion.jsonl fixtures and re-emits events
    # up to a frame playhead.
    pulp_add_test_suite(pulp-test-motion-scrubber GROUP pulp-test-group-motion-inspect
        LIBRARIES pulp::view pulp::inspect)

    pulp_add_test_suite(pulp-test-control-motion-executor GROUP pulp-test-group-motion-inspect
        LIBRARIES pulp::view pulp::inspect
        LABELS "inspect;control;motion;t1;main-thread")
endif()

# Motion end-to-end animation smoke — mirrors what a plugin author
# would write: drive a real Tween, publish_value each tick, read the
# fixture back, assert the shape matches the configured animation.
pulp_add_test_suite(pulp-test-motion-animation-smoke GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Motion provenance adapters. One test per
# animation surface (Tween + PULP_MOTION_TWEEN, AnimatorSetBuilder::name,
# CSS TransitionSpec, ambient slot for rAF + design-import). Verifies the
# Provenance envelope round-trips end to end through the publish channel.
pulp_add_test_suite(pulp-test-motion-provenance GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Motion input record + replay end-to-end: records a hover -> click ->
# drag against a synthetic view tree, replays the
# same fixture against a fresh tree, asserts identical motion fixture
# emerges (modulo timing tolerance).
pulp_add_test_suite(pulp-test-motion-input-replay GROUP pulp-test-group-motion
    LIBRARIES pulp::view)

# Motion cost attribution — correlates per-frame render cost +
# dirty-rect area with the trace_ids that emitted on the same frame
# (carrying their provenance envelopes). The bridge probe
# test pulls real RenderPassManager + DirtyTracker stats, so this
# suite depends on pulp::render alongside pulp::view.
#
# Sanitizer builds (ASan/UBSan/TSan) disable PULP_ENABLE_GPU and so
# pulp::render is never created — skip the cost test in that case
# instead of failing the CMake generate step. The cost code itself
# is exercised by the GPU-on lanes (macOS local smoke + linux/windows
# release-path), so coverage isn't lost.
if(TARGET pulp::render)
    pulp_add_test_suite(pulp-test-motion-cost GROUP pulp-test-group-motion
        LIBRARIES pulp::view pulp::render)
endif()

# Motion Swift bridge — exercises the C ABI shims in
# apple/Sources/PulpSwift/PulpBridge.cpp without needing the Swift
# compiler. Verifies that pulp_motion_publish_value / publish_components,
# the ambient provenance slot, and register/update/detach geometry
# traces round-trip through motion::Coordinator and respect the
# off-by-default tracing gate.
add_executable(pulp-test-motion-swift-bridge
    test_motion_swift_bridge.cpp
    ${CMAKE_SOURCE_DIR}/apple/Sources/PulpSwift/PulpBridge.cpp)
target_include_directories(pulp-test-motion-swift-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/apple/Sources/PulpSwift)
target_link_libraries(pulp-test-motion-swift-bridge PRIVATE
    pulp::view pulp::state pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-swift-bridge)

# Motion Android JNI bridge — exercises the C ABI shims in
# core/platform/src/android/jni_motion.cpp directly. The JNI shims
# (`Java_com_pulp_motion_PulpMotionNative_*`) only compile under
# __ANDROID__, but the underlying C ABI (`pulp_motion_*`) compiles on
# every host so this test runs in the normal Linux/macOS CTest matrix.
# Includes the Coordinator::reset() deadlock regression test that the
# Swift bridge's sibling-struct fix is the load-bearing change for.
add_executable(pulp-test-motion-android-bridge
    test_motion_android_bridge.cpp
    ${CMAKE_SOURCE_DIR}/core/platform/src/android/jni_motion.cpp)
target_include_directories(pulp-test-motion-android-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/core/platform/include)
target_link_libraries(pulp-test-motion-android-bridge PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-android-bridge)

# Motion visual-analysis self-check.
# Skips cleanly (exit 3) when Python deps are absent so CI without
# numpy/Pillow/scikit-image doesn't false-fail.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME pulp-motion-visual-self-check
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_self_check.py
    )
    set_tests_properties(pulp-motion-visual-self-check PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
    )

    # Visual-plus extensions (grid overlay, trim, affine first→last).
    # Same SKIP_RETURN_CODE contract — runs only when numpy / Pillow /
    # scikit-image are available; optional cv2 path is exercised when
    # opencv-python is installed.
    add_test(
        NAME pulp-motion-visual-grid-self-check
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_grid_overlay.py
    )
    set_tests_properties(pulp-motion-visual-grid-self-check PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
    )

    # Motion-gated capture smoke. Exits 3 when neither screencapture
    # nor a booted simulator is available, so non-Apple CI hosts skip
    # cleanly. When the macOS capture path is usable it exercises the
    # gate logic against a static 4×4 region (gate never opens →
    # CLI returns 3, smoke wraps that into 0).
    add_test(
        NAME pulp-motion-visual-capture-smoke
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_capture_smoke.py
    )
    set_tests_properties(pulp-motion-visual-capture-smoke PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
        TIMEOUT 60
    )

    # The one registration in this set that must not skip. Every test above
    # exits 3 when a declared dependency is absent, and a ctest SKIP is
    # indistinguishable from a PASS in a green run — so a host that quietly
    # lost a wheel reports success while the checks it was built for never
    # execute. This one fails instead, and names the missing distributions.
    # Deliberately carries no SKIP_RETURN_CODE.
    add_test(
        NAME visual-python-deps-present
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/check_visual_python_deps.py
    )
    set_tests_properties(visual-python-deps-present PROPERTIES
        LABELS "motion;visual"
        TIMEOUT 60
    )

    # Coverage for the check above. Pure stdlib by construction: this is the
    # selftest of the one check that must not skip, so it must not acquire a
    # dependency that could make it skip.
    add_test(
        NAME visual-python-deps-selftest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/test_check_visual_python_deps.py
    )
    set_tests_properties(visual-python-deps-selftest PROPERTIES
        LABELS "motion;visual"
        TIMEOUT 60
    )
endif()
