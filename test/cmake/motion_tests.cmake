# Motion, provenance, bridge, and visual-analysis tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Animation tests
pulp_add_test_suite(pulp-test-animation LIBRARIES pulp::view)

# FrameClock tests
pulp_add_test_suite(pulp-test-frame-clock LIBRARIES pulp::view)

# Meter/scalar host->view source + subscription lifecycle
pulp_add_test_suite(pulp-test-meter-source LIBRARIES pulp::view)

# Motion bedrock tests.
pulp_add_test_suite(pulp-test-motion LIBRARIES pulp::view)

# MotionPreferences reduced-motion policy + animation honoring.
add_executable(pulp-test-motion-preferences test_motion_preferences.cpp)
target_link_libraries(pulp-test-motion-preferences PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-preferences)

# Motion inspector protocol-domain tests.
if(TARGET pulp::inspect)
    add_executable(pulp-test-motion-inspector test_motion_inspector.cpp)
    target_link_libraries(pulp-test-motion-inspector PRIVATE
        pulp::view pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-inspector)

    # Motion scrubber consumes .motion.jsonl fixtures and re-emits events
    # up to a frame playhead.
    add_executable(pulp-test-motion-scrubber test_motion_scrubber.cpp)
    target_link_libraries(pulp-test-motion-scrubber PRIVATE
        pulp::view pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-scrubber)
endif()

# Motion end-to-end animation smoke — mirrors what a plugin author
# would write: drive a real Tween, publish_value each tick, read the
# fixture back, assert the shape matches the configured animation.
add_executable(pulp-test-motion-animation-smoke test_motion_animation_smoke.cpp)
target_link_libraries(pulp-test-motion-animation-smoke PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-animation-smoke)

# Motion provenance adapters. One test per
# animation surface (Tween + PULP_MOTION_TWEEN, AnimatorSetBuilder::name,
# CSS TransitionSpec, ambient slot for rAF + design-import). Verifies the
# Provenance envelope round-trips end to end through the publish channel.
add_executable(pulp-test-motion-provenance test_motion_provenance.cpp)
target_link_libraries(pulp-test-motion-provenance PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-provenance)

# Motion input record + replay end-to-end: records a hover -> click ->
# drag against a synthetic view tree, replays the
# same fixture against a fresh tree, asserts identical motion fixture
# emerges (modulo timing tolerance).
add_executable(pulp-test-motion-input-replay test_motion_input_replay.cpp)
target_link_libraries(pulp-test-motion-input-replay PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-input-replay)

# Motion cost attribution — correlates per-frame render cost +
# dirty-rect area with the trace_ids that emitted on the same frame
# (carrying their provenance envelopes). The bridge probe
# test pulls real RenderPassManager + DirtyTracker stats, so this
# test target depends on pulp::render alongside pulp::view.
#
# Sanitizer builds (ASan/UBSan/TSan) disable PULP_ENABLE_GPU and so
# pulp::render is never created — skip the cost test in that case
# instead of failing the CMake generate step. The cost code itself
# is exercised by the GPU-on lanes (macOS local smoke + linux/windows
# release-path), so coverage isn't lost.
if(TARGET pulp::render)
    add_executable(pulp-test-motion-cost test_motion_cost.cpp)
    target_link_libraries(pulp-test-motion-cost PRIVATE
        pulp::view pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-cost)
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
endif()
