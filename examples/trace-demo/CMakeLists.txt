# trace-demo: offline poly-synth whose Perfetto trace reveals the one expensive
# DSP node a scalar load average hides. Builds only with PULP_TRACING=ON (its
# whole point is the `dsp` / `dsp.node` offline spans); a default build skips it.
# The offline path is GPU-free, so no Skia/Dawn dependency.

add_executable(pulp-trace-demo main.cpp)
target_link_libraries(pulp-trace-demo PRIVATE pulp::audio pulp::runtime)
