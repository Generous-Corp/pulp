# Multirate FDN reverb — engine and catalog-node suites.
#
# A focused owner manifest rather than one entry in each of two shared ones.
# The feature spans two subsystems — the engine is pulp::signal, the catalog
# node is pulp::host — so neither existing manifest owns both halves, and
# splitting the registrations leaves nowhere that says "this is the reverb's
# test surface".
#
# It also keeps feature ownership explicit. Sampler interpolation benchmark
# registrations likewise live in their own focused manifest so unrelated test
# changes cannot invalidate its content-addressed performance evidence.

# The engine. Decay/density/bandwidth claims are measured with the reverb
# metrics in test/support plus the shared spectrum analyzers; the long
# stability sweep is a hidden [fdn-fuzz-full] case, so the default run stays
# inside a normal suite budget.
pulp_add_test_suite(pulp-test-fdn-reverb
    SOURCES test_fdn_reverb.cpp test_fdn_reverb_acceptance.cpp
            test_fdn_reverb_block_engine.cpp test_classic_reverb.cpp
    LIBRARIES pulp::signal pulp::audio-analysis
    TEST_SPEC "~[slow]~[performance]"
    TIMEOUT 900)
pulp_scaled_test_timeout(_pulp_fdn_slow_timeout 900)
catch_discover_tests(pulp-test-fdn-reverb
    TEST_SPEC "[slow]"
    TEST_PREFIX "slow::"
    LABELS slow
    PROPERTIES TIMEOUT "${_pulp_fdn_slow_timeout}")
# Instruction-count budgets: a retired-instruction count is steadier than wall
# time, but still moves with the toolchain and the host, so these run off the
# required gate under the performance label.
catch_discover_tests(pulp-test-fdn-reverb
    TEST_SPEC "[performance]"
    TEST_PREFIX "performance::"
    LABELS performance)

# The catalog node. Same RT-probe wiring as the lo-fi catalog: proves every
# declared knob reaches the baked DSP over the production injection path, and
# that a live tank-rate change — which re-derives every delay length and
# coefficient in the engine — still allocates nothing.
# The lane primitives the FDN, classic reverb and meter kernels run on: the
# plain-struct fallback (what MSVC builds) against the vector lanes, op by op.
pulp_add_test_suite(pulp-test-simd-lanes-portable
    SOURCES test_simd_lanes_portable.cpp
    LIBRARIES pulp::signal)
# The block engines end to end on the plain-struct lanes: the FDN reference
# fingerprint, decay, density and partition exactness, the classic tier and
# the meter's vector/sample parity. Its own binary, because the macro changes
# inline definitions.
pulp_add_test_suite(pulp-test-fdn-reverb-portable-lanes
    SOURCES test_fdn_reverb_block_engine.cpp test_classic_reverb.cpp
            test_multi_channel_meter_block.cpp
    LIBRARIES pulp::signal pulp::audio-analysis pulp::audio
    TEST_SPEC "~[performance]")
target_compile_definitions(pulp-test-fdn-reverb-portable-lanes PRIVATE PULP_SIMD_LANES_PORTABLE)
# The plain-struct path's partition exactness assumes, as with MSVC's default
# /fp:precise, that a scalar tail's a * b + c is computed unfused, like the
# struct's separate operations; build it that way here.
target_compile_options(pulp-test-fdn-reverb-portable-lanes PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-ffp-contract=off>)

add_executable(pulp-test-fdn-reverb-catalog test_forge_fdn_reverb_catalog.cpp)
target_sources(pulp-test-fdn-reverb-catalog PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-fdn-reverb-catalog
    PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-fdn-reverb-catalog)
