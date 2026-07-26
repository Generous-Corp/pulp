# Sampler interpolation evaluator benchmark and its content-addressed evidence.
#
# Keep this focused manifest in the verifier's source bundle. The previous
# coupling to app_audio_host_tests.cmake made unrelated host/UI registrations
# invalidate a measurement of unchanged sampler code.

if(PULP_BENCHMARK)
    add_executable(pulp-sampler-interpolation-benchmark
        sample_interpolation_benchmark.cpp)
    target_link_libraries(pulp-sampler-interpolation-benchmark PRIVATE pulp::audio)
endif()

if(Python3_Interpreter_FOUND AND PULP_BENCHMARK)
    add_test(NAME sampler-interpolation-benchmark-evidence
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/scripts/verify_sampler_interpolation_benchmark.py"
            --benchmark-binary $<TARGET_FILE:pulp-sampler-interpolation-benchmark>)
    add_test(NAME sampler-interpolation-benchmark-evidence-self-test
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/scripts/verify_sampler_interpolation_benchmark.py"
            --self-test
            --benchmark-binary $<TARGET_FILE:pulp-sampler-interpolation-benchmark>)
    set_tests_properties(
        sampler-interpolation-benchmark-evidence
        sampler-interpolation-benchmark-evidence-self-test
        PROPERTIES LABELS "audio;sampler;bench;evidence")
elseif(Python3_Interpreter_FOUND)
    add_test(NAME sampler-interpolation-benchmark-source-evidence
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/scripts/verify_sampler_interpolation_benchmark.py"
            --source-only)
    add_test(NAME sampler-interpolation-benchmark-source-evidence-self-test
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/scripts/verify_sampler_interpolation_benchmark.py"
            --self-test --source-only)
    set_tests_properties(
        sampler-interpolation-benchmark-source-evidence
        sampler-interpolation-benchmark-source-evidence-self-test
        PROPERTIES LABELS "audio;sampler;bench;evidence;source-only")
endif()
