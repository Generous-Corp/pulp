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
)

target_link_libraries(pulp-osc PUBLIC pulp::runtime)

if(WIN32)
    target_link_libraries(pulp-osc PRIVATE ws2_32)
endif()
