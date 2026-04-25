# UI Preview — standalone app for validating the rendering pipeline.
# Desktop-only: depends on pulp::inspect which needs the GPU stack.
if(APPLE AND NOT PULP_IOS AND PULP_ENABLE_GPU AND TARGET pulp::inspect)
    add_executable(pulp-ui-preview main.cpp)
    target_link_libraries(pulp-ui-preview PRIVATE
        pulp::view
        pulp::state
        pulp::inspect
    )

    # Zero-copy benchmark mode (Slice 0 of #516 / #517). The root
    # CMakeLists.txt adds PULP_BENCHMARK as a compile definition
    # globally when the option is ON — ui-preview just needs an
    # explicit link against pulp::render so its perf-counter header
    # is on the include path.
    if(PULP_BENCHMARK)
        if(TARGET pulp-render)
            target_link_libraries(pulp-ui-preview PRIVATE pulp::render)
        endif()
    endif()
endif()
