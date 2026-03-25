# Pulp example projects
# Each example validates a phase of the framework

# Phase 2-3: PulpGain (trivial gain plugin)
add_subdirectory(pulp-gain)

# Phase 4: PulpTone (oscillator synth + musical typing)
add_subdirectory(pulp-tone)

# Phase 5: PulpEffect (filter with parameters and state)
add_subdirectory(pulp-effect)

# Phase 5: PulpCompressor (sidechain compressor validating multi-bus)
add_subdirectory(pulp-compressor)

# Phase 6: UI Preview (validates rendering pipeline)
add_subdirectory(ui-preview)

# Phase 6: PulpDrums (generative drum sequencer MIDI effect)
add_subdirectory(PulpDrums)

# Phase 7: PulpSynth (macro oscillator with DSP library)
add_subdirectory(PulpSynth)

# Phase 9: PulpSampler (YouTube audio sampler)
# add_subdirectory(pulp-sampler)
