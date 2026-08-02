add_library(pulp-timeline-agent-view
    src/agent_view.cpp)
add_library(pulp::timeline-agent-view ALIAS pulp-timeline-agent-view)

target_compile_features(pulp-timeline-agent-view PUBLIC cxx_std_20)
target_include_directories(pulp-timeline-agent-view
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-timeline-agent-view PUBLIC pulp::timeline pulp::runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline-agent-view PRIVATE -fno-exceptions -fno-rtti)
endif()
