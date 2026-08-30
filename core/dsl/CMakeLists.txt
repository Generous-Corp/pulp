# core/dsl — DSL integration subsystem (FAUST, future: Cmajor, JSFX)
# Header-only: the contract and processor wrappers are templates.

add_library(pulp-dsl INTERFACE)
add_library(pulp::dsl ALIAS pulp-dsl)

target_include_directories(pulp-dsl INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# dsl_processor.hpp includes exactly one format header, pulp/format/processor.hpp,
# which is the authoring vocabulary and nothing else. Linking the umbrella here
# pulled the whole editor surface -- and through it core/view's Skia and Dawn
# archives -- into every DSL consumer that never asked for a UI.
target_link_libraries(pulp-dsl INTERFACE pulp::format-core)
