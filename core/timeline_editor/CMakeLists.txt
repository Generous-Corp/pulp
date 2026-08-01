# core/timeline_editor — the editor rung's interfaces.
#
# Header-only, and the link list is the contract: an editor may reach the
# document model and the timebase, and nothing else. `pulp::playback` in
# particular is absent by design — a view learns where the playhead is through
# SequencerUiHost, whose implementation lives with whoever owns audio.
# tools/scripts/timeline_engine_dependency_floor_check.py enforces that closure
# over both the includes here and the links below.
#
# `pulp::view` and `pulp::canvas` are absent for the same reason, and a view
# target landing beside this one is not a reason to add them: it takes its own
# directory and its own floor row, above this one. Widening this list instead
# would make every consumer of the editor kernel — including a plugin that only
# wants the interfaces — pay for the view stack.

add_library(pulp-timeline-editor INTERFACE)
add_library(pulp::timeline-editor ALIAS pulp-timeline-editor)

target_compile_features(pulp-timeline-editor INTERFACE cxx_std_20)

target_include_directories(pulp-timeline-editor INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline-editor INTERFACE
    pulp::timeline
    pulp::timebase
)
