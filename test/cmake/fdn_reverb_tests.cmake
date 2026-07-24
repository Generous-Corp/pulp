# Multirate FDN reverb — engine and catalog-node suites.
#
# A focused owner manifest rather than one entry in each of two shared ones.
# The feature spans two subsystems — the engine is pulp::signal, the catalog
# node is pulp::host — so neither existing manifest owns both halves, and
# splitting the registrations leaves nowhere that says "this is the reverb's
# test surface".
#
# It also keeps the feature clear of a fragile coupling: app_audio_host_tests
# .cmake is one of seven paths inside the sampler interpolation benchmark's
# hashed source bundle (tools/scripts/verify_sampler_interpolation_benchmark
# .py), so any unrelated target added there invalidates recorded benchmark
# evidence nobody re-measured. That is a pre-existing hazard, not this
# feature's, but there is no reason to walk into it.

# The engine. Decay/density/bandwidth claims are measured with the reverb
# metrics in test/support plus the shared spectrum analyzers; the long
# stability sweep is a hidden [fdn-fuzz-full] case, so the default run stays
# inside a normal suite budget.
pulp_add_test_suite(pulp-test-fdn-reverb
    LIBRARIES pulp::signal pulp::audio-analysis
    TIMEOUT 900)

# The catalog node. Same RT-probe wiring as the lo-fi catalog: proves every
# declared knob reaches the baked DSP over the production injection path, and
# that a live tank-rate change — which re-derives every delay length and
# coefficient in the engine — still allocates nothing.
add_executable(pulp-test-fdn-reverb-catalog test_fdn_reverb_catalog.cpp)
target_sources(pulp-test-fdn-reverb-catalog PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-fdn-reverb-catalog
    PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-fdn-reverb-catalog)
