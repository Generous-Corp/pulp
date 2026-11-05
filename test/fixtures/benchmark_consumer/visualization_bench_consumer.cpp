// The PULP_BENCHMARK consumer shape of VisualizationBridge, as
// examples/ui-preview uses it: the bridge reports its audio-copy and publish
// timings into a render::bench::PerfCounters snapshot through the sink.
//
// Compiled with PULP_BENCHMARK and never linked. Every ordinary build compiles
// it, so a change to the benchmark-only API fails here instead of only in the
// opt-in benchmark builds. It is an object, not an executable, because the
// libraries it would link are built without PULP_BENCHMARK, and the bridge's
// layout differs between the two.

#ifndef PULP_BENCHMARK
#error "the benchmark consumer check must be compiled with PULP_BENCHMARK"
#endif

#include <pulp/render/bench/perf_counters.hpp>
#include <pulp/view/visualization_bridge.hpp>

void pulp_visualization_bench_consumer_check(pulp::view::VisualizationBridge& bridge,
                                             pulp::render::bench::PerfCounters& counters) {
    bridge.set_bench_sink({&counters.audio_copy_total_us, &counters.triplebuffer_publish_total_us});
    bridge.set_bench_sink({});
}
