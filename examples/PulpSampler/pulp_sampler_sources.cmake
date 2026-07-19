# Compile these sources in each consumer. PULP_SAMPLER_TEST_HOOKS deliberately
# changes private class layout, so production and hook-enabled targets must not
# share precompiled sampler implementation objects.
set(PULP_SAMPLER_IMPLEMENTATION
    ${CMAKE_CURRENT_LIST_DIR}/pulp_sampler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pulp_sampler_render.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sampler_streaming_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sampler_streaming_service.cpp)
