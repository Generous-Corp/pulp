# pulp-view-graph-editor — GraphEditorView, the audio-UI widget that draws and
# edits the plug-in host's SignalGraph. It is the one view target that depends
# on pulp::host, so the view umbrellas do not link it: a consumer that draws a
# host graph links pulp::view-graph-editor, and an ordinary view or plug-in UI
# never pulls in the plug-in host stack. Defined in its own directory so that
# host edge is the graph editor's alone. The host is not built on iOS (no
# third-party plug-in hosting there), so neither is this target; its header
# still installs with the view headers. pulp::host is defined later
# (core/host); the link resolves at generate time.
#
# Migration: an installed-SDK consumer that used GraphEditorView through
# Pulp::host now links Pulp::view-graph-editor.
if(PULP_IOS)
    return()
endif()

add_library(pulp-view-graph-editor src/graph_editor_view.cpp)
add_library(pulp::view-graph-editor ALIAS pulp-view-graph-editor)
target_include_directories(pulp-view-graph-editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-view-graph-editor PUBLIC pulp::view-core pulp::host)
