// Headless screenshot capture for the GPU NAM editor.
// Builds the processor, loads the default model, pushes a short noise burst so
// the live spectrum display has content, builds the editor via create_view(), and
// renders to a PNG with no GPU window and no audio device.
//
//   gpu-nam-shot [out.png] [--raster|--gpu]
//
//   GN_GPU=1   route the AUDIO through the GPU engine (kEngine=1); blocks are
//              paced at real-time so the non-RT GPU worker gets the wall-clock gap
//              it has in a live host.

#include "gpu_nam_processor.hpp"  // pulls in BufferView + MidiBuffer via processor.hpp
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

    const char* out = "/tmp/gpu_nam_ui.png";
    bool want_gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--gpu") want_gpu = true;
        else if (arg == "--raster") want_gpu = false;
        else if (!arg.empty() && arg[0] != '-') out = argv[i];
    }

    state::StateStore store;
    examples::GpuNamProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(examples::kInputGain, 6.0f);
    store.set_value(examples::kOutputGain, 0.0f);
    store.set_value(examples::kMix, 100.0f);
    store.set_value(examples::kBypass, 0.0f);
    const bool gpu_engine = std::getenv("GN_GPU") != nullptr;
    store.set_value(examples::kEngine, gpu_engine ? 1.0f : 0.0f);

    constexpr int BLOCK = 512;
    constexpr double SR = 48000.0;
    format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = BLOCK;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    std::uint32_t s = 0xC0FFEE11u;
    std::vector<float> l(BLOCK), r(BLOCK), ol(BLOCK), orr(BLOCK);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = SR;
    pctx.num_samples = BLOCK;
    const int nblocks = gpu_engine ? 64 : 8;
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
        if (gpu_engine) std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long>(BLOCK / SR * 1e6)));
    }
    const auto status = proc.gpu_status();
    std::printf("GPU NAM: engine=%s backend=%s blocks=%llu avg_us=%.0f rt%%=%.0f model=%s\n",
                status.active ? "GPU" : "CPU", status.backend.c_str(),
                static_cast<unsigned long long>(status.blocks), status.avg_us,
                status.rt_percent, proc.model_name().c_str());

    ScreenshotBackend backend = ScreenshotBackend::skia;
    if (want_gpu) {
        if (view::has_gpu_capture()) backend = ScreenshotBackend::gpu;
        else std::printf("GPU NAM: --gpu requested but no GPU capture backend; "
                         "falling back to CPU raster.\n");
    }

    auto v = proc.create_view();
    const bool ok = view::render_to_file(*v, 820, 560, out, 2.0f, backend);
    std::printf("GPU NAM editor screenshot [%s]: %s -> %s\n",
                backend == ScreenshotBackend::gpu ? "gpu" : "raster",
                ok ? "OK" : "FAILED", out);
    proc.release();
    return ok ? 0 : 1;
}
