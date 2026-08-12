# core/timeline_view — the arranger rung: a canvas over a document, and the
# gestures that edit it.
#
# The link list is the contract. This rung may reach the editor kernel below it
# and the view/canvas stack beside it, and nothing else. `pulp::playback` is
# absent by design: a view learns where the playhead is through
# SequencerUiHost, whose implementation lives with whoever owns audio.
# `pulp::project_package` is absent for the same reason in the other direction
# — storage is a sibling rung, not a base of the editor, so an arranger is
# proven against serialize round-trip rather than acquiring a package format.
# tools/scripts/timeline_engine_dependency_floor_check.py enforces that closure
# over both the includes here and the links below.
#
# This is its own directory and its own floor row rather than a widening of
# core/timeline_editor's, exactly as that target's comment instructs: admitting
# view there would make every consumer of the editor kernel — including a
# plugin that wants only the interfaces — pay for the view stack.

add_library(pulp-timeline-view
    src/arranger_view.cpp
    src/piano_roll_view.cpp)
add_library(pulp::timeline-view ALIAS pulp-timeline-view)

target_compile_features(pulp-timeline-view PUBLIC cxx_std_20)

target_include_directories(pulp-timeline-view
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline-view
    PUBLIC
        pulp::timeline-editor
        pulp::timeline
        pulp::timebase
        pulp::view-core
        pulp::canvas
)
