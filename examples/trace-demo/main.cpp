// trace-demo — the "the load meter said 40%, the trace said WHY" demo.
//
// A tiny offline poly synth whose point is that a Perfetto trace reveals what a
// single scalar load average hides. Most voices are cheap sine oscillators; ONE
// voice ("lead") routes through a deliberately expensive 8x-oversampled
// nonlinear waveshaper. At moderate polyphony the average cost looks calm, but
// the `dsp.node` spans show one fat span eating each block while its siblings
// are slivers.
//
// It renders OFFLINE (core/audio's offline_process path), which is deadline-free
// and deterministic — the only audio path Perfetto may instrument (plan §0c D1:
// TRACE_EVENT is NOT real-time-safe, so the live process() callback is never
// traced). The render is wrapped in a process-global Tracing session and flushes
// a .pftrace you open in ui.perfetto.dev.
//
// Build requires PULP_TRACING=ON (the example is CMake-gated on it); a default
// build never compiles Perfetto in.

#include <pulp/audio/offline_processor.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

// A cheap sine voice bank (a held chord) — trivial per sample.
struct Voice {
    double freq_hz;
    double phase = 0.0;
};

// Advance one voice by `frames` samples into `out` (mono, additive).
void render_sine(Voice& v, float* out, int frames, float gain) {
    const double inc = 2.0 * M_PI * v.freq_hz / kSampleRate;
    for (int i = 0; i < frames; ++i) {
        out[i] += gain * static_cast<float>(std::sin(v.phase));
        v.phase += inc;
        if (v.phase > 2.0 * M_PI) v.phase -= 2.0 * M_PI;
    }
}

// The expensive node: 8x-oversampled cubic soft-clip on the lead voice. The
// oversampling + per-sample transcendental is the point — it dominates the
// block even though the load meter's average stays comfortable.
void render_lead_oversampled(Voice& v, float* out, int frames, float gain) {
    constexpr int kOversample = 8;
    const double inc = 2.0 * M_PI * v.freq_hz / (kSampleRate * kOversample);
    for (int i = 0; i < frames; ++i) {
        float acc = 0.0f;
        for (int os = 0; os < kOversample; ++os) {
            float x = static_cast<float>(std::sin(v.phase)) * 3.0f;
            // Cubic soft clip + a transcendental to make the node genuinely fat.
            float shaped = x - (x * x * x) / 6.0f;
            shaped = std::tanh(shaped);
            acc += shaped;
            v.phase += inc;
            if (v.phase > 2.0 * M_PI) v.phase -= 2.0 * M_PI;
        }
        out[i] += gain * (acc / static_cast<float>(kOversample));
    }
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 1.0;
    std::string out_path =
        (std::filesystem::temp_directory_path() / "pulp-trace-demo.pftrace")
            .string();
    if (argc > 1) seconds = std::atof(argv[1]);
    if (argc > 2) out_path = argv[2];
    if (seconds <= 0.0) seconds = 1.0;

    const uint64_t frames =
        static_cast<uint64_t>(seconds * kSampleRate);

    // Silent stereo input; the synth writes its output in the callback. Offline
    // render iterates blocks over the input length.
    pulp::audio::AudioFileData input;
    input.sample_rate = static_cast<uint32_t>(kSampleRate);
    input.channels.assign(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));

    // A held minor chord: three cheap voices + one expensive lead.
    std::vector<Voice> cheap = {{220.0}, {261.63}, {329.63}};
    Voice lead{440.0};

    printf("trace-demo: rendering %.2fs offline (%llu frames)...\n", seconds,
           static_cast<unsigned long long>(frames));

    // Only the offline audio path is traced (never live process()).
    pulp::runtime::Tracing::start({"dsp", "dsp.node"}, out_path);

    auto rendered = pulp::audio::offline_process(
        input,
        [&](const float* /*in*/, float* out, int channels, int block_size,
            double /*sr*/) {
            std::vector<float> mono(static_cast<size_t>(block_size), 0.0f);

            {
                // Cheap: all sine voices summed. Slivers in the flamegraph.
                PULP_TRACE_SCOPE_NAMED("dsp.node", "voice_oscillators");
                for (auto& v : cheap)
                    render_sine(v, mono.data(), block_size, 0.2f);
            }
            {
                // Expensive: the one oversampled node. The fat span.
                PULP_TRACE_SCOPE_NAMED("dsp.node", "lead_oversampler");
                render_lead_oversampled(lead, mono.data(), block_size, 0.3f);
            }
            {
                // Cheap: fan the mono mix out to every channel.
                PULP_TRACE_SCOPE_NAMED("dsp.node", "mix_bus");
                for (int f = 0; f < block_size; ++f)
                    for (int ch = 0; ch < channels; ++ch)
                        out[static_cast<size_t>(f) * channels + ch] = mono[f];
            }
        },
        kBlockSize);

    auto stop = pulp::runtime::Tracing::stop();

    if (!rendered) {
        fprintf(stderr, "trace-demo: offline render failed\n");
        return 1;
    }
    if (!stop.ok) {
        fprintf(stderr, "trace-demo: trace flush failed\n");
        return 1;
    }

    printf("trace-demo: wrote %s (%llu bytes)\n", stop.path.c_str(),
           static_cast<unsigned long long>(stop.trace_bytes));
    printf("trace-demo: open it in https://ui.perfetto.dev — the "
           "'lead_oversampler' span dominates each block while its siblings are "
           "slivers.\n");
    return 0;
}
