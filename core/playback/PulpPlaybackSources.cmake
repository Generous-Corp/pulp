include_guard(GLOBAL)

function(pulp_resolve_playback_sources root output)
    set(_PULP_PLAYBACK_SOURCE_FILES
        audio_renderer.cpp
        audio_renderer_converter_cache.cpp
        audio_renderer_render.cpp
        automation_cursor.cpp
        automation_program.cpp
        automation_program_compiler.cpp
        automation_recording.cpp
        capture_engine.cpp
        chord_pattern_renderer.cpp
        compile_context_registry.cpp
        compile_executor.cpp
        external_sync_output.cpp
        external_sync_timecode.cpp
        generated_event_source.cpp
        midi_capture_materializer.cpp
        note_renderer.cpp
        offline_stretch_artifact.cpp
        offline_stretch_program_compiler.cpp
        production_class.cpp
        program.cpp
        program_compiler.cpp
        program_compiler_helpers.cpp
        program_validator.cpp
        program_wire.cpp
        recording_commit.cpp
        recording_coordinator.cpp
        realtime_stretch_state_bank.cpp
        realtime_stretch_renderer.cpp
        sequence_compile_bookkeeping.cpp
        sequence_content_lowerer.cpp
        sequence_dirty_lowering.cpp
        sequence_preflight.cpp
        stable_renderer_shell.cpp
        tempo_sync.cpp
        track_automation_compiler.cpp
        track_automation_program.cpp
        track_automation_renderer.cpp
        track_audio_program_compiler.cpp
        transport.cpp
    )
    set(_PULP_PLAYBACK_SOURCES)
    foreach(_source IN LISTS _PULP_PLAYBACK_SOURCE_FILES)
        list(APPEND _PULP_PLAYBACK_SOURCES "${root}/core/playback/src/${_source}")
    endforeach()
    set(${output} ${_PULP_PLAYBACK_SOURCES} PARENT_SCOPE)
endfunction()
