# Creative Timeline model, playback, host-binding, architecture, and example
# validation. Keeping these registrations together prevents timeline changes
# from invalidating evidence bundles owned by unrelated subsystems.

pulp_add_test_suite(pulp-test-timeline-model
    SOURCES test_timeline_model.cpp test_timeline_device_placement.cpp
        test_timeline_automation_attachment.cpp
        test_timeline_note_modifiers.cpp
        test_timeline_take_comp.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-dawproject-import
    SOURCES test_timeline_dawproject_import.cpp
        test_timeline_dawproject_import_runtime.cpp
    LIBRARIES pulp::playback pulp::dawproject-import)

# Bounded DAWproject export. Registered with the dawproject subsystem rather
# than the interchange hub: a format adapter owns its own tests, and this one
# needs the importer as its round-trip oracle.
pulp_add_test_suite(pulp-test-dawproject-export
    SOURCES test_dawproject_export.cpp
    LIBRARIES pulp::dawproject-export pulp::dawproject-import pulp::interchange
        pulp::timeline)
target_compile_definitions(pulp-test-timeline-dawproject-import PRIVATE
    PULP_TIMELINE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")
# pulp::midi supplies the shared MIDI 2.0 velocity scaling the SMF interop
# module reimplements under its -fno-exceptions contract; linking it here keeps
# the two provably in agreement.
pulp_add_test_suite(pulp-test-timeline-smf
    SOURCES test_timeline_smf.cpp
    LIBRARIES pulp::smf-interop pulp::midi)
pulp_add_test_suite(pulp-test-smf-interchange
    SOURCES test_smf_interchange.cpp
    LIBRARIES pulp::smf-interchange pulp::smf-interop pulp::interchange
    INCLUDE_DIRS ${choc_SOURCE_DIR})
pulp_add_test_suite(pulp-test-timeline-production-mode
    SOURCES test_timeline_production_mode.cpp
    LIBRARIES pulp::timeline)
# SequencerUiHost is the editor rung's only coupling toward playback, so the
# link list is part of what this suite proves: it names the editor interface and
# the document model, and never pulp::playback. A member of the interface that
# grew into an engine type would fail to build here.
pulp_add_test_suite(pulp-test-sequencer-ui-host
    SOURCES test_sequencer_ui_host.cpp test_timeline_viewport_projection.cpp
    LIBRARIES pulp::timeline-editor pulp::timeline)
# Names both rungs at once, which neither rung may do for itself. The link list
# is the point: standing above the transport and the editor is what makes "both
# describe a loop with one type" a statement the build checks, rather than a
# claim two headers make separately and can drift apart on.
pulp_add_test_suite(pulp-test-timebase-loop-region
    LIBRARIES pulp::playback pulp::timeline-editor pulp::timeline)
pulp_add_test_suite(pulp-test-playback-production
    SOURCES test_playback_production_class.cpp
        test_playback_buffered_content_source.cpp
        test_playback_generated_event_source.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::audio pulp::timeline pulp::native-components
        ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
# Links the view layer as well: the mouse/touch parity fixture drives the
# device-dependent hit metrics through the pointer-neutral intent seam. That
# link is the test's alone — it stands where a front-end stands, above both
# rungs, and is exactly what neither pulp::timeline-editor nor pulp::timeline
# is allowed to name.
pulp_add_test_suite(pulp-test-timeline-edit-intents
    SOURCES test_timeline_edit_intents.cpp
    LIBRARIES pulp::timeline-editor pulp::timeline pulp::view)
pulp_add_test_suite(pulp-test-timeline-automation-curve LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-automation-lane LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-playback-transport
    SOURCES test_playback_transport.cpp test_playback_transport_epoch.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::format ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-capture-engine
    SOURCES test_playback_capture_engine.cpp test_playback_recording_commit.cpp
        test_playback_automation_recording.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-standalone-recording
    SOURCES test_standalone_recording.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::standalone pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-external-sync
    SOURCES test_playback_external_sync.cpp
    LIBRARIES pulp::playback)
pulp_add_test_suite(pulp-test-playback-tempo-sync
    SOURCES test_playback_tempo_sync.cpp
    LIBRARIES pulp::playback)

# The SDK-present lane is always visible in ctest. A stock build returns 77 and
# is reported as SKIPPED with the exact opt-in knobs; an SDK-enabled build links
# and executes the real adapter instead of silently omitting the test.
add_executable(pulp-test-ableton-link-sdk test_ableton_link_sdk.cpp)
if(TARGET pulp::ableton-link)
    target_link_libraries(pulp-test-ableton-link-sdk PRIVATE pulp::ableton-link)
else()
    target_link_libraries(pulp-test-ableton-link-sdk PRIVATE pulp::playback)
endif()
add_test(NAME playback-ableton-link-sdk-present COMMAND pulp-test-ableton-link-sdk)
set_tests_properties(playback-ableton-link-sdk-present PROPERTIES
    LABELS "playback;vendor-sdk"
    SKIP_RETURN_CODE 77)
pulp_add_test_suite(pulp-test-playback-program
    SOURCES test_playback_program.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS
        $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>
        $<$<BOOL:${PULP_SANITIZER}>:PULP_TEST_WITH_SANITIZER=1>)
pulp_add_test_suite(pulp-test-timeline-nesting
    SOURCES test_timeline_nesting_model.cpp
        test_timeline_nesting_playback.cpp
        harness/rt_allocation_probe.cpp
    LIBRARIES pulp::playback pulp::audio-analysis pulp::audio pulp::timeline pulp::timebase
        pulp::runtime)
if(PULP_BENCHMARK)
    set(PULP_TIMELINE_SCALE_SANITIZED OFF)
    if(PULP_SANITIZER OR CMAKE_CXX_FLAGS MATCHES "(^|[ ;])-fsanitize")
        set(PULP_TIMELINE_SCALE_SANITIZED ON)
    endif()
    pulp_add_test_suite(pulp-test-timeline-scale
        SOURCES test_timeline_scale.cpp
        LIBRARIES pulp::playback
        LABELS performance
        TIMEOUT 120
        COMPILE_DEFINITIONS
            $<$<BOOL:${PULP_TIMELINE_SCALE_SANITIZED}>:PULP_TEST_WITH_SANITIZER=1>)
endif()
pulp_add_test_suite(pulp-test-playback-note-renderer
    SOURCES test_playback_note_renderer.cpp
        test_playback_note_modifiers.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-audio-renderer
    SOURCES test_playback_audio_renderer.cpp
        test_playback_audio_renderer_conversion.cpp
        test_playback_offline_stretch.cpp
        test_playback_stretch_lifecycle.cpp
        test_playback_realtime_stretch_state_bank.cpp
        test_playback_track_freeze.cpp
        test_playback_track_mixer.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::audio-analysis pulp::audio pulp::timeline pulp::timebase
        pulp::runtime pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-automation-cursor
    SOURCES test_playback_automation_cursor.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-track-automation-program
    SOURCES test_playback_track_automation_program.cpp
    LIBRARIES pulp::playback)
pulp_add_test_suite(pulp-test-playback-track-automation-renderer
    SOURCES test_playback_track_automation_renderer.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-clip-launch
    SOURCES test_playback_clip_launch.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)

if(Python3_Interpreter_FOUND)
    add_test(NAME timeline-transaction-launcher-complexity
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_transaction_launcher_complexity.py
            ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction_scene_internal.cpp)
    add_test(NAME timeline-transaction-launcher-complexity-mutation-control
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_transaction_launcher_complexity.py
            --self-test)
    add_test(NAME timeline-launcher-bulk-build-contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_launcher_resource_contract.py
            --launcher-source
            ${CMAKE_SOURCE_DIR}/core/timeline/src/sequence_scene_internal.cpp)
    add_test(NAME timeline-launcher-retained-size-contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_launcher_resource_contract.py
            --command-source
            ${CMAKE_SOURCE_DIR}/core/timeline/src/command.cpp)
    add_test(NAME timeline-launcher-resource-contract-mutation-control
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_launcher_resource_contract.py
            --self-test)
    add_test(NAME timeline-live-capture-verifier-self-test
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_live_capture.py
            --self-test)
    add_test(NAME timeline-live-capture-hardware
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_live_capture.py)
    set_tests_properties(timeline-live-capture-hardware PROPERTIES
        LABELS "hardware;validation"
        SKIP_RETURN_CODE 77)
    add_test(NAME timeline-sync-soak-verifier-self-test
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_sync_soak.py
            --self-test)
    add_test(NAME timeline-sync-hardware-soak
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_sync_soak.py)
    set_tests_properties(timeline-sync-hardware-soak PROPERTIES
        LABELS "hardware;validation"
        SKIP_RETURN_CODE 77)
endif()

pulp_add_test_suite(pulp-test-timeline-commands
    SOURCES test_timeline_commands.cpp test_timeline_automation_commands.cpp
        test_timeline_take_commands.cpp test_timeline_track_freeze.cpp
        test_timeline_marker_commands.cpp test_timeline_track_mixer.cpp
        test_timeline_track_commands.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-transactions LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-note-transform LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-journal
    SOURCES test_timeline_journal.cpp test_timeline_file_journal.cpp
        harness/rt_allocation_probe.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-undo LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-schema-registry LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-schema-codegen LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-agent
    SOURCES test_timeline_agent.cpp
    LIBRARIES pulp::tool-timeline pulp::audio pulp::timeline)
# The chord/scale context lane plus the compile-context subscription contract
# it carries: the document type, its schema migrations, and the read side that
# only resolves context a renderer declared.
pulp_add_test_suite(pulp-test-timeline-context-lane
    SOURCES test_timeline_context_lane.cpp
    LIBRARIES pulp::timeline)
# The groove a sequence plays with, carried on the same contract: the swing and
# step-table transform, the document type and its migrations, and the read side
# that resolves a groove only for a renderer that declared it.
pulp_add_test_suite(pulp-test-timeline-groove
    SOURCES test_timeline_groove.cpp
    LIBRARIES pulp::timeline)
# The invalidation side of the same contract, driven through the real program
# compiler so "did not recompile" is observed rather than assumed.
pulp_add_test_suite(pulp-test-playback-compile-context
    SOURCES test_playback_compile_context.cpp
    LIBRARIES pulp::playback)
pulp_add_test_suite(pulp-test-timeline-persistence
    SOURCES test_timeline_persistence.cpp
        test_timeline_automation_persistence.cpp
        test_timeline_asset_loop_info.cpp
        test_timeline_command_persistence.cpp
        test_timeline_device_placement_persistence.cpp
        test_timeline_midi_content.cpp
        test_timeline_note_modifier_persistence.cpp
        test_timeline_marker_persistence.cpp
        test_timeline_session_persistence.cpp
        test_timeline_persistence_limits.cpp
        test_timeline_persistence_registry.cpp
        test_timeline_release_serialization.cpp
        test_timeline_take_comp_persistence.cpp
        test_timeline_tuning.cpp
    LIBRARIES pulp::timeline)
target_compile_definitions(pulp-test-timeline-persistence PRIVATE
    PULP_TIMELINE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")
pulp_add_test_suite(pulp-test-timeline-replay-golden
    SOURCES test_timeline_replay_golden.cpp
    LIBRARIES pulp::playback)
target_compile_definitions(pulp-test-timeline-replay-golden PRIVATE
    PULP_TIMELINE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")

pulp_add_test_suite(pulp-test-timeline-graph-binding
    SOURCES test_timeline_graph_binding.cpp
        test_timeline_graph_automation_delivery.cpp
        test_timeline_graph_binding_lifecycle.cpp
        test_timeline_graph_binding_publication.cpp
        test_host_transport_projector.cpp
        test_sequence_processor.cpp
        test_sequence_stretch_alignment.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::host pulp::sequence pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)

# MIDI controller -> parameter value scaling, and the DoD proof that a hardware
# CC maps to any parameter with zero driver code. The routing/learn foundation
# lives in core/state (pulp::state); the proof links pulp::timeline to drive the
# engine's own DeviceParameterTarget parameter identity, showing the engine's
# automatable parameters are the same ParamID space the map already reaches.
pulp_add_test_suite(pulp-test-midi-parameter-map-scaling
    SOURCES test_midi_parameter_map_scaling.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::state pulp::timeline)

# The OSC counterpart of the same surface: an OSC address (literal or OSC 1.0
# wildcard pattern) binds to the same ParamID space, so a tablet control surface
# reaches the engine's DeviceParameterTarget parameters with zero driver code.
# The binding lives in core/osc (pulp::osc) because it needs the OSC address
# matcher; pulp::timeline is linked to drive the engine's own parameter identity.
pulp_add_test_suite(pulp-test-osc-parameter-map
    SOURCES test_osc_parameter_map.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::osc pulp::state pulp::timeline)

# The multitrack/PDC proof is also registered separately from the aggregate
# example suite. That keeps the Phase-2 gate independent of standalone-editor
# linkage and lets no-JS engine builds exercise the proof directly.
pulp_add_test_suite(pulp-test-timeline-multitrack-pdc
    SOURCES
        ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_multitrack_arrangement.cpp
        ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_multitrack_arrangement.cpp
        harness/rt_allocation_probe.cpp
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/timeline-phase1
        ${CMAKE_SOURCE_DIR}/test
    LIBRARIES pulp::format pulp::host pulp::playback pulp::timeline
        pulp::timebase pulp::native-components ${CMAKE_DL_LIBS})

# Worked example (d): the clip-launching session. Engine-only (no pulp::host),
# so it exercises launch quantization, per-track provider arbitration, and the
# capture flatten on every platform the document model builds on.
pulp_add_test_suite(pulp-test-timeline-launch-session
    SOURCES
        ${CMAKE_SOURCE_DIR}/examples/timeline-session/timeline_launch_session.cpp
        ${CMAKE_SOURCE_DIR}/examples/timeline-session/test_timeline_launch_session.cpp
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/timeline-session
    LIBRARIES pulp::playback pulp::timeline pulp::timebase)

# Worked example (e): the full DAW-style project. Hybrid per-track arbitration,
# nested SequenceRef with one diverged copy, take lanes with a comp, an agent
# batch, and journal-backed autosave — all on one document.
pulp_add_test_suite(pulp-test-timeline-daw-project
    SOURCES
        ${CMAKE_SOURCE_DIR}/examples/timeline-session/timeline_daw_project.cpp
        ${CMAKE_SOURCE_DIR}/examples/timeline-session/test_timeline_daw_project.cpp
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/timeline-session
    LIBRARIES pulp::playback pulp::timeline pulp::timebase)

# The portable conformance runner over this corpus is built by
# core/interchange, which is configured on every platform — including the mobile
# lanes where this test directory is not added at all. Only the ctest wiring and
# the corpus itself live here; the corpus stays under test/ because it is the
# fixture data the rest of this suite already loads.
if(NOT TARGET pulp-fixture-runner)
    # Dropping these two registrations silently would remove the corpus gate
    # while every lane stayed green, which is the exact blindness the gate
    # exists to prevent. Fail the configure instead.
    message(FATAL_ERROR
        "pulp-fixture-runner is missing: core/interchange must be configured "
        "before test/ for the timeline fixture corpus gate to be registered.")
endif()
add_test(NAME timeline-fixture-corpus
    COMMAND pulp-fixture-runner --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")

# The corpus run above proves the runner passes on a good corpus. It cannot
# prove the runner FAILS on a bad one, and a conformance gate that cannot go red
# is indistinguishable from no gate. This suite shells out to the binary against
# deliberately broken temporary corpora and asserts each exit code and message.
pulp_add_test_suite(pulp-test-fixture-runner-cli
    SOURCES test_fixture_runner_cli.cpp
    LIBRARIES pulp::platform)
add_dependencies(pulp-test-fixture-runner-cli pulp-fixture-runner)
target_compile_definitions(pulp-test-fixture-runner-cli PRIVATE
    PULP_FIXTURE_RUNNER_BINARY="$<TARGET_FILE:pulp-fixture-runner>"
    PULP_TIMELINE_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")

if(Python3_Interpreter_FOUND)
    # Playback is engine-core: format/host/view may consume it, but it may not
    # include or link back upward. The selftest proves every forbidden layer is
    # detected in both source includes and CMake linkage.
    add_test(NAME timeline-engine-dependency-floor COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_engine_dependency_floor_check.py")
    add_test(NAME timeline-engine-dependency-floor-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_engine_dependency_floor_check.py"
        --selftest)

    # A construct a user can author and the compiler then refuses leaves the
    # document unplayable with nothing saying so at authoring time, which is
    # worse than the construct not existing. Every such refusal must carry a
    # written reason and an owner. The selftest proves the check names a
    # refusal dropped from the allowlist, names a newly added one, and still
    # passes a refusal that reads nothing a document can carry.
    add_test(NAME playback-negative-capability COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/negative_capability_check.py"
        --repo-root "${CMAKE_SOURCE_DIR}")
    add_test(NAME playback-negative-capability-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/negative_capability_check.py"
        --selftest)

    add_test(NAME web-timeline-source-closure
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/web_timeline_source_closure_check.py
            --root ${CMAKE_SOURCE_DIR})
    add_test(NAME web-timeline-source-closure-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/test_web_timeline_source_closure_check.py)

    # Schema-drift gate: the committed manifest must match a fresh emission from
    # the registry. The selftest proves the gate catches a stale artifact.
    add_test(NAME timeline-schema-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_schema.json
            --emit-cmd $<TARGET_FILE:pulp-timeline-schema-emit>)
    add_test(NAME timeline-schema-drift-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/test_schema_drift_check.py)

    # TypeScript-type surface: a pure projection of the committed schema
    # manifest into a .d.ts, guarded by the same shared drift gate. The
    # committed .d.ts must match a fresh emission from the manifest; the
    # selftest proves the projection is complete and that the gate catches a
    # mutated artifact.
    add_test(NAME timeline-schema-ts-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_types.d.ts
            --emit-cmd "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/core/timeline/tools/schema_ts_emit.py --manifest ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_schema.json")
    add_test(NAME timeline-schema-ts-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/core/timeline/tools/test_schema_ts_emit.py)

    # CLI-verb surface: a pure projection of the committed schema manifest into a
    # verb/flag JSON table, guarded by the same shared drift gate. The committed
    # artifact must match a fresh emission from the manifest; the selftest proves
    # the projection is complete and that the gate catches a mutated artifact.
    add_test(NAME timeline-schema-cli-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_cli_verbs.json
            --emit-cmd "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/core/timeline/tools/schema_cli_emit.py --manifest ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_schema.json")
    add_test(NAME timeline-schema-cli-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/core/timeline/tools/test_schema_cli_emit.py)

    # JS-facade surface: a pure projection of the committed schema manifest into
    # a frozen ES module (the runtime-JS counterpart to the .d.ts), guarded by
    # the same shared drift gate. The committed module must match a fresh
    # emission from the manifest; the selftest proves the projection is complete
    # and that the gate catches a mutated artifact.
    add_test(NAME timeline-schema-js-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_facade.js
            --emit-cmd "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/core/timeline/tools/schema_js_emit.py --manifest ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_schema.json")
    add_test(NAME timeline-schema-js-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/core/timeline/tools/test_schema_js_emit.py)

    # MCP tool-defs drift gate: same shared drift check, pointed at the MCP
    # projection's emitter + committed artifact.
    add_test(NAME timeline-mcp-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_mcp_tools.json
            --emit-cmd "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/core/timeline/tools/schema_mcp_emit.py --manifest ${CMAKE_SOURCE_DIR}/core/timeline/schema/timeline_schema.json")
    add_test(NAME timeline-mcp-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/core/timeline/tools/test_schema_mcp_emit.py)
endif()

include(${CMAKE_SOURCE_DIR}/core/timeline/PulpTimelineSources.cmake)
pulp_resolve_timeline_sources(
    "${CMAKE_SOURCE_DIR}" _PULP_TIMELINE_NO_EXCEPTIONS_SOURCES)
add_library(pulp-test-timeline-no-exceptions OBJECT
    ${_PULP_TIMELINE_NO_EXCEPTIONS_SOURCES})
target_link_libraries(pulp-test-timeline-no-exceptions PRIVATE
    pulp::runtime pulp::timebase)
target_include_directories(pulp-test-timeline-no-exceptions PRIVATE
    ${CMAKE_SOURCE_DIR}/core/timeline/include)
if(MSVC)
    target_compile_options(pulp-test-timeline-no-exceptions PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(pulp-test-timeline-no-exceptions PRIVATE
        -fno-exceptions -fno-rtti)
endif()

# Compile the worked examples even when PULP_BUILD_EXAMPLES=OFF so ordinary PR
# and coverage lanes continue to exercise their real implementation sources.
add_executable(pulp-test-timeline-phase1-examples
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_example_engine.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_audio_player.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_multitrack_arrangement.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_step_pattern_content.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_step_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_examples.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_codec.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_edits.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_standalone.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_multitrack_arrangement.cpp
    ${CMAKE_SOURCE_DIR}/test/harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-timeline-phase1-examples PRIVATE
    pulp::format pulp::host pulp::playback pulp::timeline pulp::timebase
    pulp::standalone Catch2::Catch2WithMain)
target_include_directories(pulp-test-timeline-phase1-examples PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1
    ${CMAKE_SOURCE_DIR}/test)
catch_discover_tests(pulp-test-timeline-phase1-examples)

# The Timeline API-contract checker only ever runs inside build-api-docs.sh,
# which needs Doxygen and therefore runs in the docs lanes rather than the test
# suite. That left the checker's own logic — the exemptions for internal
# namespaces, destructors, defaulted and deleted members, and the public-header
# path filter — with nothing asserting it still discriminates. An exemption that
# widens by accident turns the gate silent while every lane stays green.
#
# The self-test needs no Doxygen: it drives check() over synthetic compounddef
# XML and asserts both directions, so it belongs in the normal suite.
add_test(NAME timeline-api-docs-check-selftest
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/scripts/timeline_api_docs_check.py --self-test)

# Engine-side half of the reference-clock sync soak, owned by this subsystem.
include("${CMAKE_CURRENT_LIST_DIR}/sync_soak_engine.cmake")
