# Multi-character delay — signal engine and host catalog suites.
#
# The feature owns its registration here rather than app_audio_host_tests.cmake:
# that broad manifest is part of the sampler interpolation benchmark's hashed
# source bundle, so unrelated target edits invalidate recorded measurements.

pulp_add_test_suite(pulp-test-character-delay
    SOURCES
        test_character_delay.cpp
        test_character_delay_characters.cpp
        test_character_delay_controls.cpp
        test_character_delay_physical.cpp
    LIBRARIES pulp::signal pulp::native-components pulp::runtime ${CMAKE_DL_LIBS}
    TEST_SPEC "~[slow]"
    TIMEOUT 900)
pulp_scaled_test_timeout(_pulp_character_delay_slow_timeout 900)
catch_discover_tests(pulp-test-character-delay
    TEST_SPEC "[slow]"
    TEST_PREFIX "slow::"
    LABELS slow
    PROPERTIES TIMEOUT "${_pulp_character_delay_slow_timeout}")
target_sources(pulp-test-character-delay PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)

# test_forge_character_delay_catalog.cpp is registered once, as
# pulp-test-forge-character-delay-catalog in dsp_series_modules.cmake with the
# rest of the DSP series. A second suite over the same source registers every
# case twice under the same ctest name, and protected_merge_receipt.py refuses
# an inventory with a duplicated name, which silently disabled receipt reuse.
