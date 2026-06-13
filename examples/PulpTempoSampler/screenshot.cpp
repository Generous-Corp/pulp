// Headless screenshot of the PulpTempoSampler editor (Phase 5 Ink & Signal UX).
// Loads a synthetic multi-onset loop, builds the editor via create_view(), and
// renders to a PNG with the CPU Skia raster backend — no GPU window, no audio.
//
//   pulp-tempo-sampler-shot [out.png]

#include "pulp_tempo_sampler.hpp"
#include <pulp/view/screenshot.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    using namespace pulp;
    constexpr double pi = 3.14159265358979323846;

    state::StateStore store;
    examples::PulpTempoSamplerProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    // Synthetic loop: 4 decaying percussive bursts so the waveform shows real
    // shape and the onset detector yields slice regions.
    const long sr = 48000, n = sr * 2; // 2 s
    const long beat = n / 4;
    std::vector<float> buf(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
        const double t = static_cast<double>(i % beat) / static_cast<double>(beat);
        const double env = std::exp(-9.0 * t);
        const double freq = 90.0 + 50.0 * static_cast<double>((i / beat) % 3);
        buf[static_cast<size_t>(i)] =
            static_cast<float>(0.85 * env * std::sin(2.0 * pi * freq * i / sr));
    }
    const float* ch[1] = {buf.data()};
    proc.load_loop(ch, 1, n, static_cast<double>(sr));

    auto v = proc.create_view();
    const char* out = (argc > 1) ? argv[1] : "/tmp/pulp_tempo_sampler_ui.png";
    const bool ok = view::render_to_file(*v, 720, 240, out, 2.0f, view::ScreenshotBackend::skia);
    std::printf("PulpTempoSampler editor screenshot: %s (bpm=%.1f, slices=%zu) -> %s\n",
                ok ? "OK" : "FAILED", proc.detected_bpm(), proc.num_slices(), out);
    return ok ? 0 : 1;
}
