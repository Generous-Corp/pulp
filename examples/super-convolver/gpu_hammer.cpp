// GPU-only load generator: issues back-to-back multi-IR GPU convolution submits
// with NO CPU convolution work between them, so the GPU duty cycle is high and a
// system GPU-utilization counter (e.g. ioreg "Device Utilization %") clearly
// rises. This exists to prove, at the hardware level, that the convolution math
// genuinely executes on the GPU — not just that a label says "GPU".
//
//   super-convolver-gpu-hammer [seconds] [num_ir] [fft_n]
//
// Prints the Metal device, submit count, and effective submit rate. If the GPU
// device is absent it says so and exits non-zero (it never silently falls back).

#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace pulp;
    const double seconds = argc > 1 ? std::atof(argv[1]) : 8.0;
    const uint32_t num_ir = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 64u;
    const uint32_t n = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 2048u;

    auto gpu = render::GpuCompute::create();
    if (!gpu || !gpu->initialize_standalone()) {
        std::printf("GPU hammer: NO GPU DEVICE — cannot prove GPU audio here.\n");
        return 2;
    }
    const auto caps = gpu->capabilities();
    std::printf("GPU hammer: device = %s %s | %u rooms | fft n=%u\n",
                caps.vendor.c_str(), caps.backend.c_str(), num_ir, n);

    // Build num_ir distinct IR spectra (just decaying noise, frequency-domain
    // here is fine — we only care that the GPU runs the batched math).
    std::uint32_t s = 0x1234567u;
    std::vector<float> ir_specs(static_cast<size_t>(2) * n * num_ir);
    for (auto& v : ir_specs) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(s >> 9) / 4194304.0f - 1.0f;
    }
    if (!gpu->prepare_multi_convolution(n, ir_specs.data(), num_ir)) {
        std::printf("GPU hammer: prepare_multi_convolution failed (likely over the "
                    "storage-buffer binding limit at %u rooms × n=%u).\n", num_ir, n);
        return 3;
    }

    std::vector<float> in(2u * n, 0.0f), out(2u * n, 0.0f);
    std::vector<float> pan_l(num_ir, 0.7f), pan_r(num_ir, 0.7f);
    for (uint32_t i = 0; i < n; ++i) in[2u * i] = std::sin(0.05f * i);  // real input

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    uint64_t submits = 0;
    double checksum = 0.0;
    while (elapsed() < seconds) {
        if (!gpu->multi_convolve(in.data(), pan_l.data(), pan_r.data(),
                                 out.data(), n, num_ir)) {
            std::printf("GPU hammer: multi_convolve dispatch FAILED at submit %llu\n",
                        static_cast<unsigned long long>(submits));
            return 4;
        }
        checksum += out[0] + out[n];  // touch output so it can't be optimized away
        ++submits;
    }
    const double el = elapsed();
    std::printf("GPU hammer: %llu GPU submits in %.2fs = %.0f submits/s "
                "(each = forward FFT + %u complex-multiplies + %u inverse FFTs + "
                "stereo reduce, all on the GPU). checksum=%.3f\n",
                static_cast<unsigned long long>(submits), el, submits / el,
                num_ir, num_ir, checksum);
    return 0;
}
