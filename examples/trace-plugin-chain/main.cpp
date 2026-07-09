// trace-plugin-chain — profile a REAL Pulp effect chain with Perfetto.
//
// trace-demo traces synthetic DSP; this traces THREE REAL example plugins —
// PulpGain, PulpEffect (a biquad filter), and PulpCompressor — through their
// actual Processor::process() code, driven offline with HeadlessHost. One
// `dsp.node` span per plugin per block answers the everyday question "which
// plugin in my chain is expensive?" — and the real, measured answer is a
// cautionary tale about averages:
//
//   * The FIRST block shows a large spike on whichever node runs first (here
//     `gain`): the first process() call warms instruction caches and touches
//     fresh pages. It is a one-time cold-start cost, not a per-block cost.
//   * In STEADY STATE the biquad filter is the hot node (~5-6 us/block), the
//     compressor next (~4 us/block), and the gain is effectively free
//     (~0.2 us/block) — it is just a multiply.
//
// A scalar per-node average inverts this: gain's average is dominated by its
// one-time warmup and looks like the worst offender. The per-block trace
// separates the cold-start spike from the true steady-state cost. That is the
// whole point of tracing over a load meter, shown on real plugin code.
//
// Offline only: the render path is deadline-free and deterministic — the sole
// audio path Perfetto may instrument. The live process() callback is never
// traced, because TRACE_EVENT takes a lock at buffer rollover and is not
// real-time-safe. Build requires PULP_TRACING=ON; a default build never
// compiles Perfetto in.

#include <pulp/format/headless.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

#include "chain.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

// PulpCompressor / PulpEffect parameter IDs, as literals so this driver needn't
// include the plugin headers (their unscoped enums collide — see chain.hpp).
constexpr pulp::state::ParamID kCompThreshold = 1;  // dB
constexpr pulp::state::ParamID kCompRatio = 2;      // ratio
constexpr pulp::state::ParamID kFilterFreq = 1;     // Hz
constexpr pulp::state::ParamID kFilterMix = 4;      // 0..1

}  // namespace

int main(int argc, char** argv) {
    double seconds = 0.06;
    std::string out_path =
        (std::filesystem::temp_directory_path() / "pulp-trace-plugin-chain.pftrace")
            .string();
    if (argc > 1) seconds = std::atof(argv[1]);
    if (argc > 2) out_path = argv[2];
    if (seconds <= 0.0) seconds = 0.06;

    const auto frames = static_cast<std::uint64_t>(seconds * kSampleRate);
    const int blocks = static_cast<int>((frames + kBlockSize - 1) / kBlockSize);

    // Three real plugins, driven headless (no DAW, no UI, no audio device).
    pulp::format::HeadlessHost gain(&tracechain::make_gain);
    pulp::format::HeadlessHost filter(&tracechain::make_filter);
    pulp::format::HeadlessHost comp(&tracechain::make_compressor);
    gain.prepare(kSampleRate, kBlockSize, 2, 2);
    filter.prepare(kSampleRate, kBlockSize, 2, 2);
    comp.prepare(kSampleRate, kBlockSize, 2, 2);

    // Make the filter audibly active and push the compressor hard so its
    // gain-computation branch (pow/log10 per sample) actually runs — real work
    // for the trace to surface in every block.
    filter.state().set_value(kFilterFreq, 800.0f);
    filter.state().set_value(kFilterMix, 1.0f);
    comp.state().set_value(kCompThreshold, -35.0f);
    comp.state().set_value(kCompRatio, 12.0f);

    pulp::audio::Buffer<float> in(2, kBlockSize);
    pulp::audio::Buffer<float> a(2, kBlockSize);
    pulp::audio::Buffer<float> b(2, kBlockSize);
    pulp::audio::Buffer<float> out(2, kBlockSize);
    const pulp::audio::Buffer<float>& in_c = in;
    const pulp::audio::Buffer<float>& a_c = a;
    const pulp::audio::Buffer<float>& b_c = b;

    double phase = 0.0;
    const double inc = 2.0 * M_PI * 220.0 / kSampleRate;

    printf("trace-plugin-chain: profiling gain -> filter -> compressor over %d "
           "blocks (%.3fs)...\n",
           blocks, seconds);

    pulp::runtime::Tracing::start({"dsp", "dsp.node"}, out_path);

    for (int blk = 0; blk < blocks; ++blk) {
        // A loud stereo sine, well above the compressor threshold.
        for (int i = 0; i < kBlockSize; ++i) {
            const float s = 0.7f * static_cast<float>(std::sin(phase));
            in.channel(0)[i] = s;
            in.channel(1)[i] = s;
            phase += inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        auto a_v = a.view();
        auto b_v = b.view();
        auto out_v = out.view();
        auto in_v = in_c.view();
        auto a_cv = a_c.view();
        auto b_cv = b_c.view();

        {
            PULP_TRACE_SCOPE_NAMED("dsp.node", "gain");
            gain.process(a_v, in_v);
        }
        {
            PULP_TRACE_SCOPE_NAMED("dsp.node", "biquad_filter");
            filter.process(b_v, a_cv);
        }
        {
            PULP_TRACE_SCOPE_NAMED("dsp.node", "compressor");
            comp.process(out_v, b_cv);
        }
    }

    const auto stop = pulp::runtime::Tracing::stop();

    if (!stop.ok) {
        fprintf(stderr, "trace-plugin-chain: trace flush failed\n");
        return 1;
    }

    printf("trace-plugin-chain: wrote %s (%llu bytes)\n", stop.path.c_str(),
           static_cast<unsigned long long>(stop.trace_bytes));
    printf("trace-plugin-chain: open it in https://ui.perfetto.dev — the first "
           "'gain' block is a one-time cold-start spike; in steady state the "
           "'biquad_filter' node is the per-block hot spot, not the average's "
           "apparent winner.\n");
    return 0;
}
