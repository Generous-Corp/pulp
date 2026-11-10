# Shared build requirements for the portable RIFF/WAVE decoder: the chunk
# walk and acceptance policy (wave_layout.cpp) and the dr_wav frame reader
# behind it (wav_decoder.cpp).

include_guard(GLOBAL)

function(pulp_resolve_portable_wav PULP_ROOT OUT_SOURCES OUT_INCLUDES)
    set(${OUT_SOURCES}
        "${PULP_ROOT}/core/audio/src/wave_layout.cpp"
        "${PULP_ROOT}/core/audio/src/wav_decoder.cpp"
        PARENT_SCOPE)
    set(${OUT_INCLUDES}
        "${PULP_ROOT}/external/dr_libs"
        PARENT_SCOPE)
endfunction()
