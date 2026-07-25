# Forge modulation catalog (pulp::signal modulation toolkit as bake-layer nodes).
#
# Registered in its own manifest rather than in app_audio_host_tests.cmake on
# purpose: verify_sampler_interpolation_benchmark.py hashes that shared manifest
# as one of its declared source inputs, so registering an unrelated suite there
# reds the sampler evidence gate. A focused manifest keeps this suite's ownership
# visible and leaves that gate alone.
add_executable(pulp-test-forge-modulation-catalog test_forge_modulation_catalog.cpp)
target_sources(pulp-test-forge-modulation-catalog PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-forge-modulation-catalog
    PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-forge-modulation-catalog)
