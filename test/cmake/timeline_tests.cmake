# Creative Timeline model, playback, host-binding, architecture, and example
# validation. Keeping these registrations together prevents timeline changes
# from invalidating evidence bundles owned by unrelated subsystems.

pulp_add_test_suite(pulp-test-timeline-model
    SOURCES test_timeline_model.cpp test_timeline_device_placement.cpp
        test_timeline_automation_attachment.cpp
        test_timeline_take_comp.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-dawproject-import
    SOURCES test_timeline_dawproject_import.cpp
    LIBRARIES pulp::timeline)
target_compile_definitions(pulp-test-timeline-dawproject-import PRIVATE
    PULP_TIMELINE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/timeline")
pulp_add_test_suite(pulp-test-timeline-automation-curve LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-automation-lane LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-playback-transport
    SOURCES test_playback_transport.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::format ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-program
    SOURCES test_playback_program.cpp
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS
        $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>
        $<$<BOOL:${PULP_SANITIZER}>:PULP_TEST_WITH_SANITIZER=1>)
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
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::playback pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
pulp_add_test_suite(pulp-test-playback-audio-renderer
    SOURCES test_playback_audio_renderer.cpp test_playback_track_freeze.cpp
        harness/rt_allocation_probe.cpp
    LIBRARIES pulp::playback pulp::audio-analysis pulp::audio pulp::timeline pulp::timebase
        pulp::runtime)
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

pulp_add_test_suite(pulp-test-timeline-commands
    SOURCES test_timeline_commands.cpp test_timeline_automation_commands.cpp
        test_timeline_take_commands.cpp test_timeline_track_freeze.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-transactions LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-journal
    SOURCES test_timeline_journal.cpp test_timeline_file_journal.cpp
    LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-undo LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-schema-registry LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-schema-codegen LIBRARIES pulp::timeline)
pulp_add_test_suite(pulp-test-timeline-persistence
    SOURCES test_timeline_persistence.cpp
        test_timeline_automation_persistence.cpp
        test_timeline_asset_loop_info.cpp
        test_timeline_device_placement_persistence.cpp
        test_timeline_persistence_limits.cpp
        test_timeline_persistence_registry.cpp
        test_timeline_release_serialization.cpp
        test_timeline_take_comp_persistence.cpp
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
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::host pulp::native-components ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)

if(Python3_Interpreter_FOUND)
    # Playback is engine-core: format/host/view may consume it, but it may not
    # include or link back upward. The selftest proves every forbidden layer is
    # detected in both source includes and CMake linkage.
    add_test(NAME timeline-engine-dependency-floor COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_engine_dependency_floor_check.py")
    add_test(NAME timeline-engine-dependency-floor-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_engine_dependency_floor_check.py"
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
endif()

add_library(pulp-test-timeline-no-exceptions OBJECT
    ${CMAKE_SOURCE_DIR}/core/timeline/src/asset_schema_migrations.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/assets.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/automation_curve.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/automation_document_internal.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/automation_lane.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/command.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/document_session.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/identity_directory.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/id_remap.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/journal.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/model.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_codegen.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_json.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_json_canonical.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_json_parser.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_json_preflight.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_release.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_json_validation.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/schema_registry.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_asset_loop_decode.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_automation_decode.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_decode_support.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_decode.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_encode.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/serialize_release.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/snapshot_equivalence.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/structural_registry_validation.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/track.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/track_schema_migrations.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/take_lane_schema_migrations.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/take_lane.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction_automation_internal.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction_take_internal.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction_track_state_internal.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/transaction_reduction_support.cpp
    ${CMAKE_SOURCE_DIR}/core/timeline/src/undo.cpp)
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
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_step_pattern_content.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/timeline_step_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_examples.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_codec.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_edits.cpp
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1/test_timeline_phase1_standalone.cpp
    ${CMAKE_SOURCE_DIR}/test/harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-timeline-phase1-examples PRIVATE
    pulp::format pulp::host pulp::playback pulp::timeline pulp::timebase
    pulp::standalone Catch2::Catch2WithMain)
target_include_directories(pulp-test-timeline-phase1-examples PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/timeline-phase1
    ${CMAKE_SOURCE_DIR}/test)
catch_discover_tests(pulp-test-timeline-phase1-examples)
