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
        compile_executor.cpp
        external_sync_output.cpp
        external_sync_timecode.cpp
        midi_capture_materializer.cpp
        note_renderer.cpp
        program.cpp
        program_compiler.cpp
        recording_commit.cpp
        stable_renderer_shell.cpp
        track_automation_compiler.cpp
        track_automation_program.cpp
        track_automation_renderer.cpp
        transport.cpp
    )
    set(_PULP_PLAYBACK_SOURCES)
    foreach(_source IN LISTS _PULP_PLAYBACK_SOURCE_FILES)
        list(APPEND _PULP_PLAYBACK_SOURCES "${root}/core/playback/src/${_source}")
    endforeach()
    set(${output} ${_PULP_PLAYBACK_SOURCES} PARENT_SCOPE)
endfunction()
