# core/dsl — DSL integration subsystem (FAUST, future: Cmajor, JSFX)
# Header-only: the contract and processor wrappers are templates.

add_library(pulp-dsl INTERFACE)
add_library(pulp::dsl ALIAS pulp-dsl)

target_include_directories(pulp-dsl INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-dsl INTERFACE pulp::format)
