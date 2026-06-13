// Headless screenshot of the PulpTempoSampler editor (Phase 5 Ink & Signal UX).
// Loads a synthetic multi-onset loop, builds the editor via create_view(), and
// renders to a PNG with no GPU window and no audio device.
//
//   pulp-tempo-sampler-shot [out.png] [--raster|--gpu]
//
//   --raster (default)  CPU Skia raster (SkSurfaces::Raster). Fast, deterministic,
//                       and faithful for this editor (it has no requires_gpu_host()
//                       views). This is the "for sure" headless path.
//   --gpu               Offscreen GPU (Dawn + Skia HeadlessSurface). Renders the
//                       SAME view tree through the real GPU stack the standalone
//                       host uses — a headless proof of the GPU render path with
//                       no live window / CVDisplayLink dependency. Falls back to
//                       raster with a notice when the build has no GPU backend.
//
// For a capture of the *live* GPU standalone host window, run the standalone
// binary with `--screenshot=out.png` (see main.cpp); that path shows a real
// window and is the live-host proof, distinct from this offscreen tool.

#include "pulp_tempo_sampler.hpp"
#include <pulp/view/screenshot.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace pulp;
    using view::ScreenshotBackend;
    constexpr double pi = 3.14159265358979323846;

    const char* out = "/tmp/pulp_tempo_sampler_ui.png";
    bool want_gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--gpu") want_gpu = true;
        else if (arg == "--raster") want_gpu = false;
        else if (!arg.empty() && arg[0] != '-') out = argv[i];
    }

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

    ScreenshotBackend backend = ScreenshotBackend::skia;
    if (want_gpu) {
        if (view::has_gpu_capture()) {
            backend = ScreenshotBackend::gpu;
        } else {
            std::printf("PulpTempoSampler: --gpu requested but this build has no "
                        "GPU capture backend; falling back to CPU raster.\n");
        }
    }

    auto v = proc.create_view();
    const bool ok = view::render_to_file(*v, 760, 372, out, 2.0f, backend);
    std::printf("PulpTempoSampler editor screenshot [%s]: %s (bpm=%.1f, slices=%zu) -> %s\n",
                backend == ScreenshotBackend::gpu ? "gpu" : "raster",
                ok ? "OK" : "FAILED", proc.detected_bpm(), proc.num_slices(), out);
    return ok ? 0 : 1;
}
