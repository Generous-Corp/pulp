add_library(pulp-host)
add_library(pulp::host ALIAS pulp-host)

target_include_directories(pulp-host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-host PRIVATE
    src/scanner.cpp
    src/signal_graph.cpp
    src/plugin_slot.cpp
)

target_link_libraries(pulp-host PUBLIC pulp::audio pulp::midi pulp::runtime)

# CLAP plugin loader + scanner (requires CLAP SDK)
if(PULP_HAS_CLAP)
    target_sources(pulp-host PRIVATE
        src/plugin_slot_clap.cpp
        src/scanner_clap.cpp
    )
    target_link_libraries(pulp-host PRIVATE clap)
    target_compile_definitions(pulp-host PRIVATE PULP_HOST_HAS_CLAP=1)
endif()
