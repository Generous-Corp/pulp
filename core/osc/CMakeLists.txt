add_library(pulp-osc)
add_library(pulp::osc ALIAS pulp-osc)

target_include_directories(pulp-osc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-osc PRIVATE
    src/osc.cpp
    src/osc_udp.cpp
    src/bundle.cpp
    src/osc_channel.cpp
)

# pulp::state is public because osc_parameter_map.hpp binds OSC addresses to
# StateStore parameters in its interface. The dependency points this way — the
# control protocol onto the parameter model — so pulp::state stays free of
# socket code and keeps its lightweight link surface for audio-thread consumers.
target_link_libraries(pulp-osc PUBLIC pulp::runtime pulp::state)

if(WIN32)
    target_link_libraries(pulp-osc PRIVATE ws2_32)
endif()
