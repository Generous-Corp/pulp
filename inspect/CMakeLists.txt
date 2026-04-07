add_library(pulp-inspect)
add_library(pulp::inspect ALIAS pulp-inspect)

target_include_directories(pulp-inspect
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-inspect PRIVATE
    src/inspector_overlay.cpp
    src/state_inspector.cpp
    src/console_capture.cpp
)

target_link_libraries(pulp-inspect PUBLIC pulp::view pulp::render pulp::state)
