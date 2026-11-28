# EditorBridge's dispatch core: handlers, JSON and WebView message dispatch and
# the response helpers. It needs only CHOC and the runtime, so it is its own
# target outside core/view, and an SDK that serves its UI from Vellum still
# builds and ships it. The header keeps its installed path,
# pulp/view/editor_bridge.hpp. The attach_native_runtime overloads that name
# view-script types live in core/view's tooling target, which links this one.
add_library(pulp-editor-bridge)
add_library(pulp::editor-bridge ALIAS pulp-editor-bridge)
target_sources(pulp-editor-bridge PRIVATE src/editor_bridge.cpp)
target_include_directories(pulp-editor-bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
target_compile_features(pulp-editor-bridge PUBLIC cxx_std_20)
target_link_libraries(pulp-editor-bridge PUBLIC pulp::runtime)
