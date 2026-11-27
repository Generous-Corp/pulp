# The examples a tree configured with PULP_UI_FROM_VELLUM_IN_TREE builds: the
# retained audio-only examples that live in this tree (class `audio-only` in the
# examples manifest). Each keeps the guard it has in CMakeLists.txt. They name
# no editor keyword, so pulp_add_plugin gives each its Vellum route: the
# generated parameter editor on Vellum, or the processor's editor_factory().
#
# The examples that select Pulp's own view editor (NATIVE_UI, GPU_UI,
# FULL_EDITOR, LITE_EDITOR, a Pulp UI_SCRIPT) or include pulp/view move to
# Vellum or are retired, and pulp_add_plugin refuses their keywords in this
# mode, so they are not configured here until they are ported.

add_subdirectory(pulp-gain)
add_subdirectory(host-bench-plugin)
add_subdirectory(pulp-tone)
add_subdirectory(cv-control)
if(TARGET pulp::host AND NOT TARGET sample-region-allpass-core)
    add_subdirectory(sample-region-allpass)
endif()
add_subdirectory(combined-bundle-demo)
add_subdirectory(pulp-compressor)
add_subdirectory(hot-reload-demo)
add_subdirectory(hot-reload-synth)
add_subdirectory(PulpDrums)
add_subdirectory(StepSequencer)
if(NOT IOS)
    add_subdirectory(timeline-phase1)
endif()
add_subdirectory(timeline-session)
add_subdirectory(PulpSequence)
add_subdirectory(PulpSynth)
add_subdirectory(mpe-synth)
add_subdirectory(pulp-multi-out)
add_subdirectory(pulp-transpose)
add_subdirectory(PulpSampler)
add_subdirectory(offline-stretch)
add_subdirectory(pulp-pluck)
add_subdirectory(faust-gain)
add_subdirectory(faust-filter)
add_subdirectory(faust-tremolo)
add_subdirectory(audio-rate-processor-node)
add_subdirectory(graph-control-event-probe)
add_subdirectory(stream-demo)
if(NOT ANDROID AND NOT IOS)
    add_subdirectory(ara-pitch-tracker)
endif()
if(PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS AND NOT WIN32)
    add_subdirectory(rust-gain)
endif()
if(APPLE AND PULP_ENABLE_GPU AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-agent-toolkit AND NOT TARGET AgentToolkitSynth_Core)
    add_subdirectory(agent-toolkit-synth)
endif()
if(PULP_TRACING)
    add_subdirectory(trace-demo)
    add_subdirectory(trace-plugin-chain)
endif()
if(IOS)
    add_subdirectory(ios-auv3-synth)
endif()
