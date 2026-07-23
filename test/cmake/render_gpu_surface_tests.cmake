# GPU surface, Skia context, headless renderer, and compute smoke tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

    add_executable(pulp-test-gpu-surface test_gpu_surface.cpp)
    target_link_libraries(pulp-test-gpu-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Skia surface test — also covers SkPicture (.skp) serialization on the
    # Graphite backend. The .skp round-trip cases include
    # Skia headers directly (SkPicture, SkPictureRecorder, SkSerialProcs,
    # SkPngEncoder), so this target needs SKIA_INCLUDE_DIRS even though
    # pulp::render carries Skia privately.
    # pulp::view for the SvgPathWidget gradient-stroke raster proof.
    add_executable(pulp-test-skia-surface test_skia_surface.cpp)
    target_link_libraries(pulp-test-skia-surface PRIVATE pulp::render pulp::view Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-skia-surface PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-skia-surface PRIVATE ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-test-skia-surface PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
    endif()
    catch_discover_tests(pulp-test-skia-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # HeadlessSurface CI wrapper: one-call offscreen Dawn/Skia helper
    # for golden-test fixtures. Compiles on every
    # build; runtime cases gate on PULP_HAS_SKIA && __APPLE__ at the
    # source level and otherwise soft-skip when no Dawn adapter is
    # available. Needs Skia headers directly for the SkPngEncoder /
    # SkPixmap symbols the wrapper uses to PNG-encode the readback.
    add_executable(pulp-test-headless-surface test_headless_surface.cpp)
    target_link_libraries(pulp-test-headless-surface PRIVATE
        pulp::render pulp::canvas Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-headless-surface PRIVATE
            PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-headless-surface PRIVATE
            ${SKIA_INCLUDE_DIRS})
    endif()
    catch_discover_tests(pulp-test-headless-surface
        ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU compute tests.
    add_executable(pulp-test-gpu-compute test_gpu_compute.cpp)
    target_link_libraries(pulp-test-gpu-compute PRIVATE pulp::render pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-compute ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU roofline / occupancy harness (tooling, not a test): drives every
    # MAC-dense compute pass and prints a ranked GMAC/s-vs-roofline + occupancy
    # table. Confirms the WaveNet one-thread-per-sample gap and its siblings.
    add_executable(pulp-gpu-roofline-harness
        "${PROJECT_SOURCE_DIR}/tools/gpu_roofline/gpu_roofline_harness.cpp")
    target_link_libraries(pulp-gpu-roofline-harness PRIVATE pulp::render pulp::signal)
