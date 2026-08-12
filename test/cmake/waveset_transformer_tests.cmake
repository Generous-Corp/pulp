# WavesetTransformer owns a focused variable-rate signal suite.
pulp_add_test_suite(pulp-test-waveset-transformer
    SOURCES test_waveset_transformer.cpp
    LIBRARIES pulp::signal pulp::native-components pulp::runtime ${CMAKE_DL_LIBS})
target_compile_definitions(pulp-test-waveset-transformer PRIVATE PULP_WAVESET_TEST_SEAMS=1)
target_sources(pulp-test-waveset-transformer PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
