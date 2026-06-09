add_library(pulp-graph)
add_library(pulp::graph ALIAS pulp-graph)

target_include_directories(pulp-graph
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-graph PRIVATE
    src/graph_runtime_plan.cpp
)

target_link_libraries(pulp-graph
    PUBLIC
        pulp::midi
        pulp::runtime
)

