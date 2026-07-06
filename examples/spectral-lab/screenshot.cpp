// Headless screenshot capture for the Spectral Lab editor.
// Builds the processor, pushes a short noise burst with Freeze latched so the
// cloud captures layers and the spectrum display has content, builds the editor
// via create_view(), and renders to a PNG with no GPU window and no audio device.
//
//   spectral-lab-shot [out.png] [--raster|--gpu]
//
//   --raster (default)  CPU Skia raster. Faithful for this editor (pure canvas
//                       drawing, no file images).
//   --gpu               Offscreen GPU (Dawn + Skia headless). Falls back to
//                       raster with a notice if the build has no GPU capture.
//
// Set SL_GPU=1 to drive the GPU audio engine (Engine=GPU); SL_LAYERS overrides
// the layer count.

#include "spectral_lab.hpp"  // pulls in BufferView + MidiBuffer via processor.hpp
#include <pulp/view/screenshot.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace pulp;
    using view::ScreenshotBackend;

    const char* out = "/tmp/spectral_lab_ui.png";
    bool want_gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--gpu") want_gpu = true;
        else if (arg == "--raster") want_gpu = false;
        else if (!arg.empty() && arg[0] != '-') out = argv[i];
    }

    const bool gpu_audio = std::getenv("SL_GPU") != nullptr;

    state::StateStore store;
    examples::SpectralLabProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(examples::kMix, 70.0f);
    store.set_value(examples::kMorph, 0.5f);
    store.set_value(examples::kSmear, 0.35f);
    store.set_value(examples::kJitter, 0.15f);
    store.set_value(examples::kEngine, gpu_audio ? 1.0f : 0.0f);
    if (gpu_audio) {
        const char* layers = std::getenv("SL_LAYERS");
        store.set_value(examples::kLayers, layers ? static_cast<float>(std::atoi(layers)) : 64.0f);
    } else {
        store.set_value(examples::kLayers, 16.0f);
    }

    constexpr int BLOCK = 512;
    constexpr double SR = 48000.0;
    format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = BLOCK;
    proc.prepare(ctx);

    // Push blocks of decaying noise, latching Freeze each block so the cloud
    // captures several layers. When capturing the GPU engine, pace the blocks at
    // ~real-time so the non-RT GPU worker gets its wall-clock gap (an offline
    // rush would otherwise report misses that never happen in a live host).
    std::uint32_t s = 0xC0FFEE11u;
    std::vector<float> l(BLOCK), r(BLOCK), ol(BLOCK), orr(BLOCK);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    const int nblocks = gpu_audio ? 96 : 24;
    for (int b = 0; b < nblocks; ++b) {
        for (int i = 0; i < BLOCK; ++i) {
            s = s * 1664525u + 1013904223u;
            const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            l[i] = r[i] = 0.4f * white;
        }
        // Toggle Freeze across blocks so several distinct layers are captured.
        store.set_value(examples::kFreeze, (b % 4 == 1) ? 1.0f : 0.0f);
        const float* inp[2] = {l.data(), r.data()};
        float* outp[2] = {ol.data(), orr.data()};
        audio::BufferView<const float> in(inp, 2, BLOCK);
        audio::BufferView<float> ob(outp, 2, BLOCK);
        proc.process(ob, in, min, mout, pctx);
        if (gpu_audio) std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long>(BLOCK / SR * 1e6)));
    }

    ScreenshotBackend backend = ScreenshotBackend::skia;
    if (want_gpu) {
        if (view::has_gpu_capture()) backend = ScreenshotBackend::gpu;
        else std::printf("SpectralLab: --gpu requested but no GPU capture backend; "
                         "falling back to CPU raster.\n");
    }

    auto v = proc.create_view();
    const bool ok = view::render_to_file(*v, 820, 560, out, 2.0f, backend);
    std::printf("SpectralLab editor screenshot [%s]: %s -> %s\n",
                backend == ScreenshotBackend::gpu ? "gpu" : "raster",
                ok ? "OK" : "FAILED", out);
    return ok ? 0 : 1;
}
