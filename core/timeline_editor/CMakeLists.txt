# core/timeline_editor — the editor rung: the seam toward playback, and the
# vocabulary a gesture speaks.
#
# The link list is the contract: an editor may reach the document model and the
# timebase, and nothing else. `pulp::playback` in particular is absent by design
# — a view learns where the playhead is through SequencerUiHost, whose
# implementation lives with whoever owns audio.
# tools/scripts/timeline_engine_dependency_floor_check.py enforces that closure
# over both the includes here and the links below.
#
# `pulp::view` and `pulp::canvas` are absent for the same reason, and a view
# target landing beside this one is not a reason to add them: it takes its own
# directory and its own floor row, above this one. Widening this list instead
# would make every consumer of the editor kernel — including a plugin that only
# wants the interfaces — pay for the view stack.
#
# The rung carries translation units rather than being header-only, because
# editor policy and lowering are definitions rather than declarations. The
# alternative — leaving them in `pulp-timeline` to keep this target INTERFACE —
# is what the floor gate is unable to police: `core/timeline` cannot include
# `pulp/timeline_editor/*`, so a document-model source reaching for a tool verb
# only fails when the verbs live up here.

add_library(pulp-timeline-editor
    src/edit_intent.cpp
    src/gesture_budget.cpp
    src/snap_grid.cpp
    src/track_edit_intent.cpp
    src/viewport_projection.cpp)
add_library(pulp::timeline-editor ALIAS pulp-timeline-editor)

target_compile_features(pulp-timeline-editor PUBLIC cxx_std_20)

target_include_directories(pulp-timeline-editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline-editor
    PUBLIC
        pulp::timeline
        pulp::timebase
)

# Matches pulp-timeline. The lowering runs inside a gesture, is reached from
# hosts compiled without exceptions, and used to be proven exception-free by
# pulp-test-timeline-no-exceptions while it lived in the portable timeline list;
# building it under the same flags keeps that proof rather than dropping it.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline-editor PRIVATE -fno-exceptions -fno-rtti)
endif()
