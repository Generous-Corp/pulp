// test_gpu_compute_kernel_override.cpp — the WGSL is causally upstream of the samples.
//
// The browser GPU-audio demo makes a claim that cannot be checked by looking at
// it: "this convolution is running as a WebGPU compute shader in your tab". The
// only way to check such a claim is to change the shader and observe the audio
// change. GpuCompute::kernel_source() / override_kernel_source() are that seam,
// and this is its native mirror — it gates the causal claim in normal CI, where
// there is no browser.
//
// The mutation is deliberately trivial and arithmetic (halve the kernel's stored
// result), so the assertion is exact rather than impressionistic: every output
// sample must be exactly half the baseline. A stub, a CPU fallback, or a
// precomputed result would all fail it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_compute.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using pulp::render::GpuCompute;

namespace {

constexpr const char* kNoGpu =
    "no GPU compute device available (Dawn not built, or no adapter)";

std::vector<float> flat_ir_spectrum(uint32_t n, float re, float im) {
    std::vector<float> spec(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        spec[i * 2] = re;
        spec[i * 2 + 1] = im;
    }
    return spec;
}

std::vector<float> ramp_input(uint32_t n, uint32_t batch) {
    std::vector<float> in(n * 2 * batch, 0.0f);
    for (uint32_t b = 0; b < batch; ++b) {
        for (uint32_t i = 0; i < n; ++i) {
            in[(b * n + i) * 2] =
                std::sin(0.05f * static_cast<float>(i) + 1.3f * static_cast<float>(b));
        }
    }
    return in;
}

bool replace_once(std::string& s, const std::string& from, const std::string& to) {
    const auto at = s.find(from);
    if (at == std::string::npos) return false;
    if (s.find(from, at + from.size()) != std::string::npos) return false;  // not unique
    s.replace(at, from.size(), to);
    return true;
}

// Halve the kernel's operand read. The broadcast complex-multiply is linear in
// `a`, so halving both components of `a` halves the product — and therefore,
// after the inverse FFT, halves every output sample. Exactly, not approximately.
bool halve_kernel_operand(std::string& wgsl) {
    return replace_once(wgsl, "let a_re = a[base];",
                              "let a_re = 0.5 * a[base];")
        && replace_once(wgsl, "let a_im = a[base + 1u];",
                              "let a_im = 0.5 * a[base + 1u];");
}

} // namespace

TEST_CASE("Mutating a kernel's WGSL changes the convolution output",
          "[render][gpu][compute][kernel-override]") {
    constexpr uint32_t N = 256, B = 2;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    const auto in = ramp_input(N, B);

    // Baseline: the built-in kernel.
    std::vector<float> baseline(N * 2 * B, 0.0f);
    {
        auto gpu = GpuCompute::create();
        if (!gpu || !gpu->initialize_standalone()) SKIP(kNoGpu);
        REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));
        REQUIRE(gpu->convolve_batch(in.data(), baseline.data(), N, B));
    }

    bool nonzero = false;
    for (float v : baseline) {
        if (std::abs(v) > 1e-3f) { nonzero = true; break; }
    }
    REQUIRE(nonzero);  // otherwise 0.5x of nothing would "pass"

    // Same device class, same plan, same input — one changed line of WGSL.
    auto gpu = GpuCompute::create();
    if (!gpu) SKIP(kNoGpu);

    const char* builtin = gpu->kernel_source("conv_bmul");
    REQUIRE(builtin != nullptr);

    std::string mutated(builtin);
    REQUIRE(halve_kernel_operand(mutated));
    REQUIRE(mutated != std::string(builtin));

    REQUIRE(gpu->override_kernel_source("conv_bmul", mutated.c_str()));
    // The override must be installed BEFORE the pipelines are compiled.
    if (!gpu->initialize_standalone()) SKIP(kNoGpu);

    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));
    std::vector<float> halved(N * 2 * B, 0.0f);
    REQUIRE(gpu->convolve_batch(in.data(), halved.data(), N, B));

    for (uint32_t i = 0; i < N * 2 * B; ++i) {
        INFO("index " << i);
        REQUIRE(std::abs(halved[i] - 0.5f * baseline[i]) < 1e-5f);
    }
}

TEST_CASE("Kernel-source seam rejects what it cannot do",
          "[render][gpu][compute][kernel-override]") {
    auto gpu = GpuCompute::create();
    if (!gpu) SKIP(kNoGpu);

    // Known labels are readable without a device — the sources are static text.
    REQUIRE(gpu->kernel_source("conv_bmul") != nullptr);
    REQUIRE(gpu->kernel_source("fft_stockham") != nullptr);
    REQUIRE(gpu->kernel_source("magnitude") != nullptr);

    REQUIRE(gpu->kernel_source("not_a_kernel") == nullptr);
    REQUIRE(gpu->kernel_source(nullptr) == nullptr);

    REQUIRE_FALSE(gpu->override_kernel_source("not_a_kernel", "// wgsl"));
    REQUIRE_FALSE(gpu->override_kernel_source("conv_bmul", nullptr));
    REQUIRE_FALSE(gpu->override_kernel_source(nullptr, "// wgsl"));

    // kernel_source() always reports the BUILT-IN text, not a pending override —
    // so a caller cannot accidentally chain mutations off its own replacement.
    const std::string before = gpu->kernel_source("conv_bmul");
    REQUIRE(gpu->override_kernel_source("conv_bmul", "// replaced"));
    REQUIRE(std::string(gpu->kernel_source("conv_bmul")) == before);

    // NOTE, measured rather than assumed: an override whose WGSL does not compile
    // does NOT make initialize_standalone() return false. Dawn creates the shader
    // module and pipeline objects eagerly and reports the compile failure
    // asynchronously through the uncaptured-error callback, so the seam surfaces a
    // bad kernel as a logged device error, not as a failed init. The seam is a
    // diagnostic, and a caller that overrides a kernel is expected to read the log.
}

TEST_CASE("A kernel override after initialization is refused, not silently ignored",
          "[render][gpu][compute][kernel-override]") {
    auto gpu = GpuCompute::create();
    if (!gpu || !gpu->initialize_standalone()) SKIP(kNoGpu);

    // Pipelines are compiled once, at initialization. An override applied now
    // would do nothing — so it is refused rather than accepted-and-ignored.
    const char* builtin = gpu->kernel_source("conv_bmul");
    REQUIRE(builtin != nullptr);
    REQUIRE_FALSE(gpu->override_kernel_source("conv_bmul", builtin));
}
