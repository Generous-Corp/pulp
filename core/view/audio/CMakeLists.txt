# pulp-view-audio — audio-UI widgets that bind to the plug-in host graph
# (GraphEditorView). The view umbrellas do not link this target: a consumer that
# draws a host graph links pulp::view-audio, so an ordinary view or plug-in UI
# never pulls in the plug-in host stack. The host is not built on iOS (no
# third-party plug-in hosting there), so neither is this target; its header
# still installs with the view headers. pulp::host is defined later
# (core/host); the link resolves at generate time.
if(PULP_IOS)
    return()
endif()

add_library(pulp-view-audio src/graph_editor_view.cpp)
add_library(pulp::view-audio ALIAS pulp-view-audio)
target_include_directories(pulp-view-audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-view-audio PUBLIC pulp::view-core pulp::host)
