# Shared build requirements for the portable RIFF/WAVE decoder.

include_guard(GLOBAL)

function(pulp_resolve_portable_wav PULP_ROOT OUT_SOURCES OUT_INCLUDES)
    set(${OUT_SOURCES}
        "${PULP_ROOT}/core/audio/src/wav_decoder.cpp"
        PARENT_SCOPE)
    set(${OUT_INCLUDES}
        "${PULP_ROOT}/external/dr_libs"
        PARENT_SCOPE)
endfunction()
