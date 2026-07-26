# Multi-character delay — signal engine and host catalog suites.
#
# The feature owns its registration here rather than app_audio_host_tests.cmake:
# that broad manifest is part of the sampler interpolation benchmark's hashed
# source bundle, so unrelated target edits invalidate recorded measurements.

pulp_add_test_suite(pulp-test-character-delay
    SOURCES
        test_character_delay.cpp
        test_character_delay_characters.cpp
        test_character_delay_physical.cpp
    LIBRARIES pulp::signal pulp::native-components pulp::runtime ${CMAKE_DL_LIBS}
    TIMEOUT 900)
target_sources(pulp-test-character-delay PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)

pulp_add_test_suite(pulp-test-character-delay-catalog
    SOURCES test_character_delay_catalog.cpp
    LIBRARIES pulp::host pulp::signal)
