# Portable Creative Timeline Engine source closure shared by the production
# WAM and WebCLAP lanes.  Keep this list centralized: either browser ABI must
# compile the same timebase, document, compiler, and renderer implementation.

include_guard(GLOBAL)

function(pulp_timeline_engine_web_sources ROOT OUT_SOURCES OUT_INCLUDES)
    set(_sources
        ${ROOT}/core/timebase/src/compiled_tempo_map.cpp
        ${ROOT}/core/timeline/src/assets.cpp
        ${ROOT}/core/timeline/src/command.cpp
        ${ROOT}/core/timeline/src/document_session.cpp
        ${ROOT}/core/timeline/src/identity_directory.cpp
        ${ROOT}/core/timeline/src/journal.cpp
        ${ROOT}/core/timeline/src/model.cpp
        ${ROOT}/core/timeline/src/schema_json.cpp
        ${ROOT}/core/timeline/src/schema_registry.cpp
        ${ROOT}/core/timeline/src/serialize.cpp
        ${ROOT}/core/timeline/src/transaction.cpp
        ${ROOT}/core/timeline/src/undo.cpp
        ${ROOT}/core/playback/src/audio_renderer.cpp
        ${ROOT}/core/playback/src/compile_executor.cpp
        ${ROOT}/core/playback/src/note_renderer.cpp
        ${ROOT}/core/playback/src/program.cpp
        ${ROOT}/core/playback/src/program_compiler.cpp
        ${ROOT}/core/playback/src/stable_renderer_shell.cpp
        ${ROOT}/core/playback/src/transport.cpp
    )
    set(_includes
        ${ROOT}/core/timebase/include
        ${ROOT}/core/timeline/include
        ${ROOT}/core/playback/include
    )
    set(${OUT_SOURCES} "${_sources}" PARENT_SCOPE)
    set(${OUT_INCLUDES} "${_includes}" PARENT_SCOPE)
endfunction()
