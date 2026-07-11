// Headless screenshot capture for the SuperConvolver editor.
// Builds the processor, prepares an IR, pushes a short noise burst so the live
// spectrum display has content, builds the editor via create_view(), and
// renders to a PNG with no GPU window and no audio device.
//
//   super-convolver-shot [out.png] [--raster|--gpu]
//
//   --raster (default)  CPU Skia raster. Faithful for this editor (pure canvas
//                       drawing, no file images).
//   --gpu               Offscreen GPU (Dawn + Skia headless). Same view tree
//                       through the real GPU stack; falls back to raster with a
//                       notice if the build has no GPU capture backend.

#include "super_convolver.hpp"  // pulls in BufferView + MidiBuffer via processor.hpp
#include "super_convolver_ui.hpp"  // SuperConvolverUi (for the SC_INFO capture hook)
#include <pulp/view/screenshot.hpp>

#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace pulp;
    using view::ScreenshotBackend;

    const char* out = "/tmp/super_convolver_ui.png";
    bool want_gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--gpu") want_gpu = true;
        else if (arg == "--raster") want_gpu = false;
        else if (!arg.empty() && arg[0] != '-') out = argv[i];
    }

    state::StateStore store;
    examples::SuperConvolverProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(examples::kSize, 1.5f);   // a roomy IR so the tail shows
    store.set_value(examples::kMix, 45.0f);
    store.set_value(examples::kGain, 0.0f);
    store.set_value(examples::kBypass, 0.0f);
    store.set_value(examples::kEngine, std::getenv("SC_GPU") ? 1.0f : 0.0f);  // SC_GPU=1 → GPU engine
    if (std::getenv("SC_GPU")) {
        const char* rooms = std::getenv("SC_ROOMS");
        store.set_value(examples::kRooms, rooms ? static_cast<float>(std::atoi(rooms)) : 64.0f);
    }

    constexpr int BLOCK = 512;
    constexpr double SR = 48000.0;
    format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = BLOCK;
    proc.prepare(ctx);

    // Push blocks of decaying noise so the spectrum bus has live content. When
    // capturing the GPU engine, pace the blocks at ~real-time (sleep one block's
    // worth between calls) so the non-RT GPU worker gets the wall-clock gap it
    // has in a live host — otherwise an offline rush reports misses that never
    // happen in real time.
    std::uint32_t s = 0xC0FFEE11u;
    std::vector<float> l(BLOCK), r(BLOCK), ol(BLOCK), orr(BLOCK);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    const bool pace = std::getenv("SC_GPU") != nullptr;
    const int nblocks = pace ? 64 : 8;
    for (int b = 0; b < nblocks; ++b) {
        for (int i = 0; i < BLOCK; ++i) {
            s = s * 1664525u + 1013904223u;
            const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            l[i] = r[i] = 0.4f * white;
        }
        const float* inp[2] = {l.data(), r.data()};
        float* outp[2] = {ol.data(), orr.data()};
        audio::BufferView<const float> in(inp, 2, BLOCK);
        audio::BufferView<float> ob(outp, 2, BLOCK);
        proc.process(ob, in, min, mout, pctx);
        if (pace) std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long>(BLOCK / SR * 1e6)));
    }

    ScreenshotBackend backend = ScreenshotBackend::skia;
    if (want_gpu) {
        if (view::has_gpu_capture()) backend = ScreenshotBackend::gpu;
        else std::printf("SuperConvolver: --gpu requested but no GPU capture backend; "
                         "falling back to CPU raster.\n");
    }

    auto v = proc.create_view();
    if (std::getenv("SC_INFO"))
        if (auto* ui = dynamic_cast<examples::SuperConvolverUi*>(v.get())) ui->open_info();
    const bool ok = view::render_to_file(*v, 820, 560, out, 2.0f, backend);
    std::printf("SuperConvolver editor screenshot [%s]: %s -> %s\n",
                backend == ScreenshotBackend::gpu ? "gpu" : "raster",
                ok ? "OK" : "FAILED", out);
    return ok ? 0 : 1;
}
