#include <catch2/catch_test_macros.hpp>

#include "detail/compact_ssm_cpu_adapter.hpp"
#include "detail/nam_tcn_adapter.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "detail/neural_cpu_kernels.hpp"
#include "detail/neural_simd.hpp"
#include "detail/recurrent_cpu_adapter.hpp"
#include "detail/tcn_cpu_adapter.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_model.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "support/fixture_root.hpp"

using namespace pulp::gpu_audio::detail;
namespace ns = pulp::gpu_audio::detail::neural_simd;

namespace {

// Null budgets. Scalar and SIMD instantiations run the same operations in the
// same order, so they must agree to float rounding. The block kernels use a
// bounded rational tanh (3e-7) where the references use libm, so the
// reference comparison uses the GPU WaveNet session's CPU-parity margin.
constexpr double kSimdNull = 1.0e-6;
constexpr double kReferenceParity = 2.0e-5;

// Sanitizer builds (ASan, TSan) run the same nulls at reduced size: the
// scalar references are ~20x slower there and must fit the hosted 120 s
// per-test timeout without a `slow` label, which the sanitizer lane
// excludes. Every family, every channel width (so every SIMD tile width),
// random 1..512 block tails and the impulses still run. Only the noise
// length, impulse count, the dilation depth of the 2x10 WaveNets (2x6) and
// the largest recurrent hidden size (64) shrink.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PULP_NEURAL_SIMD_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PULP_NEURAL_SIMD_SANITIZED 1
#endif
#endif
#if defined(PULP_NEURAL_SIMD_SANITIZED)
constexpr std::size_t kNullNoise = 4000;
constexpr int kNullImpulses = 2;
constexpr int kWaveNetDepth = 6;
constexpr std::array<std::uint32_t, 3> kRecurrentHidden{24, 13, 3};
#else
constexpr std::size_t kNullNoise = 48000;
constexpr int kNullImpulses = 8;
constexpr int kWaveNetDepth = 10;
constexpr std::array<std::uint32_t, 4> kRecurrentHidden{24, 64, 13, 3};
#endif

class Rng {
  public:
    explicit Rng(std::uint32_t seed) : state_(seed ? seed : 1u) {}
    float uniform(float scale = 1.0f) noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return scale * (static_cast<float>(state_ & 0xffffffu) / 8388608.0f - 1.0f);
    }
    std::uint32_t below(std::uint32_t n) noexcept {
        uniform();
        return state_ % n;
    }
    void fill(std::vector<float>& v, float scale) {
        for (auto& x : v)
            x = uniform(scale);
    }

  private:
    std::uint32_t state_;
};

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (!(d <= worst))
            worst = std::isnan(d) ? INFINITY : d;
    }
    return worst;
}

/// Long random stimulus followed by isolated impulses over silence, then a
/// hot (saturating) noise burst.
std::vector<float> stimulus(std::size_t noise, std::uint32_t seed, std::uint32_t spacing) {
    Rng rng(seed);
    std::vector<float> x;
    x.reserve(noise + kNullImpulses * spacing + noise / 4);
    for (std::size_t i = 0; i < noise; ++i)
        x.push_back(rng.uniform(1.0f));
    for (int k = 0; k < kNullImpulses; ++k) {
        x.push_back(k % 2 ? -1.0f : 1.0f);
        x.insert(x.end(), spacing - 1, 0.0f);
    }
    for (std::size_t i = 0; i < noise / 4; ++i)
        x.push_back(rng.uniform(6.0f));
    return x;
}

/// Runs `process(in, out, n)` over the stimulus in random 1..512 blocks.
template <class Process>
std::vector<float> run_random_blocks(Process&& process, const std::vector<float>& x,
                                     std::uint32_t seed) {
    Rng rng(seed);
    std::vector<float> y(x.size());
    std::size_t at = 0;
    while (at < x.size()) {
        const auto n = std::min<std::size_t>(1 + rng.below(512), x.size() - at);
        process(x.data() + at, y.data() + at, static_cast<std::uint32_t>(n));
        at += n;
    }
    return y;
}

template <class Process>
std::vector<float> run_fixed_blocks(Process&& process, const std::vector<float>& x,
                                    std::uint32_t block) {
    std::vector<float> y(x.size());
    for (std::size_t at = 0; at < x.size(); at += block) {
        const auto n = std::min<std::size_t>(block, x.size() - at);
        process(x.data() + at, y.data() + at, static_cast<std::uint32_t>(n));
    }
    return y;
}

// ---------------------------------------------------------------------------
// Synthetic serialized NAM WaveNet (A1) models.

struct NamArraySpec {
    int input_size, channels, head_size, kernel;
    std::vector<int> dilations;
    std::string activation = "Tanh";
    bool gated = false, head_bias = false;
};

std::vector<int> pow2_dilations(int count) {
    std::vector<int> d;
    for (int i = 0; i < count; ++i)
        d.push_back(1 << i);
    return d;
}

std::vector<NamArraySpec> nam_standard() {
    return {{1, 16, 8, 3, pow2_dilations(kWaveNetDepth)},
            {16, 8, 1, 3, pow2_dilations(kWaveNetDepth), "Tanh", false, true}};
}
std::vector<NamArraySpec> nam_lite() {
    return {{1, 12, 6, 3, pow2_dilations(kWaveNetDepth)},
            {12, 6, 1, 3, pow2_dilations(kWaveNetDepth), "Tanh", false, true}};
}
std::vector<NamArraySpec> nam_nano() {
    return {{1, 4, 2, 3, pow2_dilations(kWaveNetDepth)},
            {4, 2, 1, 3, pow2_dilations(kWaveNetDepth), "Tanh", false, true}};
}

/// Writes a NAM file with weights in the serialized order NamTcnArtifact reads.
std::string write_nam(const std::string& name, const std::vector<NamArraySpec>& arrays,
                      std::uint32_t seed, float head_scale = 0.02f, float gain = 1.0f) {
    Rng rng(seed);
    std::vector<float> w;
    auto push = [&](std::size_t count, float scale) {
        for (std::size_t i = 0; i < count; ++i)
            w.push_back(rng.uniform(scale));
    };
    std::ostringstream layers;
    for (std::size_t a = 0; a < arrays.size(); ++a) {
        const auto& s = arrays[a];
        const int out = s.gated ? 2 * s.channels : s.channels;
        const float conv = gain * 1.2f / std::sqrt(static_cast<float>(s.channels * s.kernel));
        const float one = 1.0f / std::sqrt(static_cast<float>(s.channels));
        push(static_cast<std::size_t>(s.channels) * s.input_size, 0.8f * gain);
        for (std::size_t l = 0; l < s.dilations.size(); ++l) {
            push(static_cast<std::size_t>(out) * s.channels * s.kernel, conv);
            push(out, 0.05f);
            push(out, 0.3f * gain);
            push(static_cast<std::size_t>(s.channels) * s.channels, one);
            push(s.channels, 0.05f);
        }
        push(static_cast<std::size_t>(s.head_size) * s.channels, one);
        if (s.head_bias)
            push(s.head_size, 0.05f);
        layers << (a ? "," : "") << "{\"input_size\":" << s.input_size
               << ",\"condition_size\":1,\"head_size\":" << s.head_size
               << ",\"channels\":" << s.channels << ",\"kernel_size\":" << s.kernel
               << ",\"dilations\":[";
        for (std::size_t d = 0; d < s.dilations.size(); ++d)
            layers << (d ? "," : "") << s.dilations[d];
        layers << "],\"activation\":\"" << s.activation
               << "\",\"gated\":" << (s.gated ? "true" : "false")
               << ",\"head_bias\":" << (s.head_bias ? "true" : "false") << "}";
    }
    w.push_back(head_scale);
    std::ostringstream json;
    json << "{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\",\"sample_rate\":48000,"
            "\"config\":{\"layers\":["
         << layers.str() << "],\"head\":null,\"head_scale\":";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.9g", head_scale);
    json << buffer << "},\"weights\":[";
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::snprintf(buffer, sizeof buffer, "%.9g", w[i]);
        json << (i ? "," : "") << buffer;
    }
    json << "]}";
    const auto path = std::filesystem::temp_directory_path() /
                      ("pulp-neural-simd-" + name + "-" + std::to_string(seed) + ".nam");
    std::ofstream(path) << json.str();
    return path.string();
}

std::string fixture_path() {
    return (pulp_test::fixture_root() / "test/fixtures/neural/example.nam").string();
}

std::vector<std::string> extra_models() {
    std::vector<std::string> paths;
    if (const char* env = std::getenv("PULP_NEURAL_SIMD_EXTRA_NAM")) {
        std::string list = env;
        std::size_t start = 0;
        while (start <= list.size()) {
            const auto end = list.find(':', start);
            const auto item = list.substr(start, end == std::string::npos ? end : end - start);
            if (!item.empty())
                paths.push_back(item);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    return paths;
}

std::unique_ptr<NamTcnArtifact> load_nam(const std::string& path, NamCpuKernel kernel) {
    auto artifact = std::make_unique<NamTcnArtifact>();
    std::string error;
    INFO(path << ": " << error);
    REQUIRE(artifact->load(path, &error));
    artifact->set_cpu_kernel(kernel);
    return artifact;
}

struct NamNull {
    double scalar_vs_simd = 0, reference_vs_simd = 0, partition = 0, peak = 0;
};

NamNull nam_null(const std::string& path, std::uint32_t seed) {
    auto reference = load_nam(path, NamCpuKernel::Reference);
    auto scalar = load_nam(path, NamCpuKernel::BlockScalar);
    auto simd = load_nam(path, NamCpuKernel::BlockSimd);
    REQUIRE(reference->cpu_kernel() == NamCpuKernel::Reference);
    REQUIRE(scalar->cpu_kernel() == NamCpuKernel::BlockScalar);
    REQUIRE(simd->cpu_kernel() == NamCpuKernel::BlockSimd);
    const auto x = stimulus(kNullNoise, seed, std::max(2048u, reference->receptive_field() + 64));
    const auto call = [](NamTcnArtifact& a) {
        return [&a](const float* i, float* o, std::uint32_t n) { a.process(i, o, n); };
    };
    const auto ref_y = run_random_blocks(call(*reference), x, seed + 1);
    const auto scalar_y = run_random_blocks(call(*scalar), x, seed + 2);
    const auto simd_y = run_random_blocks(call(*simd), x, seed + 3);
    simd->reset();
    const auto simd_64 = run_fixed_blocks(call(*simd), x, 64);
    NamNull result;
    result.scalar_vs_simd = max_abs_diff(scalar_y, simd_y);
    result.reference_vs_simd = max_abs_diff(ref_y, simd_y);
    result.partition = max_abs_diff(simd_y, simd_64);
    for (float v : ref_y)
        result.peak = std::max(result.peak, static_cast<double>(std::abs(v)));
    std::printf("[nam-null] %s isa=%s scalar_vs_simd=%.3g reference_vs_simd=%.3g "
                "partition=%.3g peak=%.3g samples=%zu\n",
                std::filesystem::path(path).filename().string().c_str(), ns::DefaultIsa::name,
                result.scalar_vs_simd, result.reference_vs_simd, result.partition, result.peak,
                x.size());
    return result;
}

void check_nam_null(const std::string& path, std::uint32_t seed) {
    const auto r = nam_null(path, seed);
    INFO(path);
    CHECK(r.peak > 1.0e-3); // the model actually produces signal
    CHECK(r.scalar_vs_simd <= kSimdNull);
    // Relative above unit level: the hot fixture peaks well above 1.
    CHECK(r.reference_vs_simd <= kReferenceParity * std::max(1.0, r.peak));
    CHECK(r.partition == 0.0);
}

} // namespace

TEST_CASE("bounded tanh stays within 3e-7 of std::tanh and saturates cleanly",
          "[gpu_audio][neural][simd]") {
    double worst = 0.0;
    for (float x = -12.0f; x <= 12.0f; x += 1.0f / 4096.0f)
        worst = std::max(worst, std::abs(static_cast<double>(ns::tanh_bounded(x)) -
                                         std::tanh(static_cast<double>(x))));
    CHECK(worst <= 3.0e-7);
    CHECK(ns::tanh_bounded(1.0e30f) == 1.0f);
    CHECK(ns::tanh_bounded(-INFINITY) == -1.0f);
    CHECK(ns::tanh_bounded(-0.75f) == -ns::tanh_bounded(0.75f));
    CHECK(std::abs(ns::sigmoid_bounded(0.7f) - 1.0 / (1.0 + std::exp(-0.7))) <= 1.5e-7);

    // The vector tanh must agree with the scalar one lane for lane.
    alignas(16) float in[4] = {-9.5f, -0.3f, 0.02f, 4.25f};
    alignas(16) float out[4];
    ns::DefaultIsa::store(out, ns::DefaultIsa::tanh(ns::DefaultIsa::load(in)));
    for (int i = 0; i < 4; ++i)
        CHECK(out[i] == ns::tanh_bounded(in[i]));

    // A NaN breaches the finite-input contract; both paths keep it visible
    // rather than turning it into a full-scale constant.
    alignas(16) float nan_in[4] = {NAN, 0.5f, -NAN, 2.0f};
    ns::DefaultIsa::store(out, ns::DefaultIsa::tanh(ns::DefaultIsa::load(nan_in)));
    CHECK(std::isnan(ns::tanh_bounded(NAN)));
    CHECK(std::isnan(out[0]));
    CHECK(std::isnan(out[2]));
    CHECK(out[1] == ns::tanh_bounded(0.5f));
}

TEST_CASE("NAM WaveNet block engine nulls scalar against SIMD and the per-sample reference",
          "[gpu_audio][neural][simd][nam]") {
    SECTION("checked-in fixture") {
        check_nam_null(fixture_path(), 11);
    }
    SECTION("NAM standard 16/8 channels, 2x10 layers") {
        check_nam_null(write_nam("standard", nam_standard(), 21), 21);
    }
    SECTION("NAM lite 12/6 channels") {
        check_nam_null(write_nam("lite", nam_lite(), 31), 31);
    }
    SECTION("NAM nano 4/2 channels") {
        check_nam_null(write_nam("nano", nam_nano(), 41), 41);
    }
    SECTION("gated arrays, every activation, odd widths and kernels, three arrays") {
        const std::vector<NamArraySpec> odd = {
            {1, 5, 7, 2, {1, 3, 7}, "ReLU", true, false},
            {5, 7, 3, 4, {2, 5}, "Hardtanh", false, true},
            {7, 3, 1, 3, {1, 4, 9}, "Sigmoid", true, true},
        };
        check_nam_null(write_nam("odd", odd, 51, 0.5f, 1.5f), 51);
        const std::vector<NamArraySpec> identity = {
            {1, 6, 6, 3, {1, 2}, "Identity", false, false},
            {6, 6, 1, 1, {1}, "Tanh", true, true},
        };
        check_nam_null(write_nam("identity", identity, 61, 0.1f, 0.5f), 61);
    }
    SECTION("hot standard model with unit head scale") {
        check_nam_null(write_nam("hot", nam_standard(), 71, 1.0f, 2.0f), 71);
    }
    SECTION("extra models named by PULP_NEURAL_SIMD_EXTRA_NAM") {
        for (const auto& path : extra_models())
            check_nam_null(path, 81);
    }
}

TEST_CASE("NAM block engine falls back to the reference for chains it cannot seed",
          "[gpu_audio][neural][simd][nam]") {
    // A third array wider than the second array's head reads the reference's
    // stale per-sample head scratch; the engine refuses that topology.
    const std::vector<NamArraySpec> unsupported = {
        {1, 4, 4, 3, {1, 2}}, {4, 4, 2, 3, {1, 2}}, {4, 4, 1, 3, {1}, "Tanh", false, true}};
    const auto path = write_nam("unsupported", unsupported, 91);
    auto reference = load_nam(path, NamCpuKernel::Reference);
    auto requested = load_nam(path, NamCpuKernel::BlockSimd);
    CHECK(requested->cpu_kernel() == NamCpuKernel::Reference);
    const auto x = stimulus(4096, 91, 64);
    const auto call = [](NamTcnArtifact& a) {
        return [&a](const float* i, float* o, std::uint32_t n) { a.process(i, o, n); };
    };
    CHECK(max_abs_diff(run_fixed_blocks(call(*reference), x, 64),
                       run_fixed_blocks(call(*requested), x, 64)) == 0.0);

    // Positive control: the supported prefix of the same chain runs the engine.
    const auto supported = write_nam("supported", {unsupported[0], unsupported[1]}, 92);
    CHECK(load_nam(supported, NamCpuKernel::BlockSimd)->cpu_kernel() == NamCpuKernel::BlockSimd);
}

TEST_CASE("NAM artifact adapter runs the SIMD engine without allocating",
          "[gpu_audio][neural][simd][nam]") {
    const auto path = write_nam("adapter", nam_standard(), 101);
    auto probe_artifact = load_nam(path, NamCpuKernel::Reference);
    const StreamingModelSpec spec{.model_id = "nam.simd",
                                  .architecture = "nam.wavenet.a1",
                                  .model_version = "1",
                                  .weights_hash = "simd-hash",
                                  .runtime_hash = "simd-runtime",
                                  .input_channels = 1,
                                  .output_channels = 1,
                                  .sample_rate = 48000,
                                  .block_size = 64,
                                  .feature_rate = 48000,
                                  .receptive_field_samples = probe_artifact->receptive_field(),
                                  .state_bytes = probe_artifact->state_bytes(),
                                  .state_schema = "nam-wavenet-a1-causal-v1",
                                  .state_schema_version = 1};
    NamTcnArtifactAdapter adapter(spec, path);
    REQUIRE(adapter.prepare({.spec = &adapter.spec(),
                             .artifact_id = "simd.nam",
                             .artifact_hash = "simd-hash",
                             .max_frames = 512}));
    auto direct = load_nam(path, NamCpuKernel::BlockSimd);
    const auto x = stimulus(8192, 101, 2048);
    std::vector<float> y(x.size()), expected(x.size());
    for (std::size_t at = 0; at < x.size(); at += 128) {
        const float* in_ptr[] = {x.data() + at};
        float* out_ptr[] = {y.data() + at};
        const pulp::audio::BufferView<const float> in(in_ptr, 1, 128);
        pulp::audio::BufferView<float> out(out_ptr, 1, 128);
        pulp::test::ScopedRtProcessProbe probe;
        adapter.process_cpu(in, out, 128, {.epoch = 1, .sequence = at / 128});
        REQUIRE(probe.allocation_count() == 0);
    }
    direct->process(x.data(), expected.data(), static_cast<std::uint32_t>(x.size()));
    CHECK(max_abs_diff(y, expected) == 0.0);
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
}

#if defined(PULP_GPU_AUDIO_WAVENET_RUNTIME)
// The shadow must stay inside the GPU session's CPU-parity margin, since a
// missed GPU block is replaced by the shadow's output.
TEST_CASE("NAM SIMD shadow matches the GPU WaveNet session within its parity margin",
          "[gpu_audio][neural][simd][nam][gpu]") {
    for (const auto& path : {fixture_path(), write_nam("gpu-standard", nam_standard(), 111)}) {
        auto simd = load_nam(path, NamCpuKernel::BlockSimd);
        auto reference = load_nam(path, NamCpuKernel::Reference);
        std::vector<pulp::gpu_audio::GpuWaveNetLayerDescriptor> layers;
        std::vector<std::vector<std::uint32_t>> dilations;
        std::vector<float> weights;
        pulp::gpu_audio::GpuWaveNetDescriptor descriptor;
        REQUIRE(reference->make_gpu_wavenet_descriptor(64, descriptor, layers, dilations, weights));
        const auto created = pulp::gpu_audio::GpuWaveNetSession::create(
            {.descriptor = descriptor, .weights = weights, .slots = 2});
        if (!created.session)
            SKIP("GPU WaveNet provider unavailable (error=" << static_cast<int>(created.error)
                                                            << ")");
        auto& session = *created.session;
        const auto x = stimulus(64 * 1500, 111, 2048);
        const auto blocks = x.size() / 64;
        std::vector<float> gpu(blocks * 64), shadow(blocks * 64), exact(blocks * 64);
        for (std::size_t b = 0; b < blocks; ++b) {
            const std::span<const float> in(x.data() + b * 64, 64);
            REQUIRE(session.submit_block(in, b + 1));
            bool received = false;
            for (int attempt = 0; attempt < 4000 && !received; ++attempt) {
                session.service(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()));
                std::array<float, 64> out{};
                if (const auto done = session.receive(out)) {
                    REQUIRE(done->status == pulp::gpu_audio::GpuWaveNetBlockStatus::GpuDelivered);
                    REQUIRE(done->sequence == b + 1);
                    std::copy(out.begin(), out.end(), gpu.begin() + b * 64);
                    received = true;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
            REQUIRE(received);
            simd->process(in.data(), shadow.data() + b * 64, 64);
            reference->process(in.data(), exact.data() + b * 64, 64);
        }
        REQUIRE(session.release());
        const auto gpu_vs_shadow = max_abs_diff(gpu, shadow);
        const auto gpu_vs_reference = max_abs_diff(gpu, exact);
        std::printf("[gpu-null] %s gpu_vs_simd=%.3g gpu_vs_reference=%.3g samples=%zu\n",
                    std::filesystem::path(path).filename().string().c_str(), gpu_vs_shadow,
                    gpu_vs_reference, gpu.size());
        CHECK(gpu_vs_shadow <= kReferenceParity);
        CHECK(gpu_vs_reference <= kReferenceParity);
    }
}

// True inside a macOS virtual machine (hw.model "VirtualMac..."), where the
// GPU is Apple's paravirtual Metal device. The required gate runs there.
static bool virtual_mac_host() {
#if defined(__APPLE__)
    char model[64] = {};
    std::size_t size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) != 0)
        return false;
    return std::string_view(model).starts_with("VirtualMac");
#else
    return false;
#endif
}

// The public WaveNet node on a real provider: when the session can be created
// on this host, the node must prepare its GPU path, report it, and deliver GPU
// blocks that null against the CPU model within the parity margin.
TEST_CASE("a public WaveNet node runs its GPU path when the provider is available",
          "[gpu_audio][neural][nam][gpu]") {
    using namespace pulp::gpu_audio;
    constexpr std::uint32_t kChannels = 2, kBlock = 64, kLead = 4, kBlocks = 400;
    const auto path = fixture_path();
    auto descriptor_source = load_nam(path, NamCpuKernel::Reference);
    std::vector<GpuWaveNetLayerDescriptor> layers;
    std::vector<std::vector<std::uint32_t>> dilations;
    std::vector<float> weights;
    GpuWaveNetDescriptor descriptor;
    REQUIRE(descriptor_source->make_gpu_wavenet_descriptor(kBlock, descriptor, layers, dilations,
                                                           weights));
    const GpuWaveNetSession::Config session{
        .descriptor = descriptor, .weights = weights, .slots = 4};
    {
        auto probe = GpuWaveNetSession::create(session);
        if (!probe)
            SKIP("GPU WaveNet provider unavailable (error=" << static_cast<int>(probe.error)
                                                            << ")");
        REQUIRE(probe.session->release());
    }
    std::array<std::unique_ptr<NamTcnArtifact>, kChannels> shadows, oracles;
    std::array<GpuNeuralCpuKernel, kChannels> kernels{};
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        shadows[ch] = load_nam(path, NamCpuKernel::BlockSimd);
        oracles[ch] = load_nam(path, NamCpuKernel::Reference);
        kernels[ch] = {shadows[ch].get(),
                       [](void* state) noexcept {
                           static_cast<NamTcnArtifact*>(state)->reset();
                           return true;
                       },
                       [](void* state, const float* in, float* out, std::uint32_t n) noexcept {
                           static_cast<NamTcnArtifact*>(state)->process(in, out, n);
                       },
                       [](void* state) noexcept { static_cast<NamTcnArtifact*>(state)->reset(); }};
    }
    GpuNeuralModelRequest request;
    request.family = NeuralFamily::WaveNet;
    request.manifest = {.model_id = "fixture.example",
                        .architecture = "nam.wavenet",
                        .model_version = "1",
                        .artifact_id = "example.nam",
                        .artifact_sha256 =
                            "66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d",
                        .license = "MIT",
                        .runtime = "pulp.nam.v1",
                        .artifact_size_bytes = 1024,
                        .sample_rate_hz = 48000,
                        .redistributable = true};
    request.block_size = kBlock;
    request.channel_kernels = kernels;
    request.wavenet_gpu = &session;
    auto model = make_gpu_neural_model(request);
    REQUIRE(model);
    GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                .model = model.model,
                                .channels = kChannels,
                                .lead_blocks = kLead,
                                .capacity = kLead + 4,
                                .shadow_policy = GpuNeuralRealtimeNode::ShadowPolicy::Inline});
    REQUIRE(node.prepare());
    // The session was created here, so the node must be on the GPU: a
    // Degraded CPU-only node fails this test rather than passing a branch.
    REQUIRE(node.gpu_kernel_available());
    auto status = node.status_snapshot();
    REQUIRE(validate_gpu_audio_status(status).accepted());
    REQUIRE(status.provider == GpuAudioProvider::Dawn);
    REQUIRE(status.selected_engine == GpuAudioEngine::Gpu);
    REQUIRE(status.provider_state == GpuAudioProviderState::Ready);
    REQUIRE(status.recovery_reason == GpuAudioRecoveryReason::None);
    REQUIRE(node.epoch() > 0);

    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {.ring_blocks = kLead + 2}));
    const auto x = stimulus(kBlocks * kBlock, 211, 4096);
    std::array<std::vector<float>, kChannels> out;
    std::array<float, kBlock> in_block{};
    std::array<std::array<float, kBlock>, kChannels> out_block{};
    for (std::uint32_t b = 0; b < kBlocks; ++b) {
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(b) * kBlock, kBlock, in_block.begin());
        std::array<const float*, kChannels> ins{in_block.data(), in_block.data()};
        std::array<float*, kChannels> outs{out_block[0].data(), out_block[1].data()};
        pulp::audio::BufferView<const float> input(ins.data(), kChannels, kBlock);
        pulp::audio::BufferView<float> output(outs.data(), kChannels, kBlock);
        transport.process(input, output, kBlock);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            out[ch].insert(out[ch].end(), out_block[ch].begin(), out_block[ch].end());
        // Give the provider a real chance at every slot before the next callback.
        for (int i = 0; i < 20; ++i) {
            transport.pump();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    const auto delivery = transport.delivery_snapshot();
    status = node.status_snapshot();
    std::printf("[gpu-node] gpu_blocks=%llu fallback_blocks=%llu missed=%llu\n",
                static_cast<unsigned long long>(delivery.gpu_blocks),
                static_cast<unsigned long long>(status.fallback_blocks),
                static_cast<unsigned long long>(status.missed_blocks));
    REQUIRE(delivery.gpu_blocks > 0);
    // A closed epoch means the node latched a recovery mid-run; name it, so a
    // failure on one host says why GPU delivery stopped.
    INFO("recovery_reason=" << static_cast<int>(node.recovery_reason()) << " recovery_sequence="
                            << node.recovery_sequence() << " missed=" << status.missed_blocks
                            << " fallback=" << status.fallback_blocks);
    // Every emitted block, GPU or shadow, nulls against the model delayed by
    // the lead. This runs before the VM skip below, so garbage GPU output
    // from before a fence still fails, and REQUIRE stops before that skip.
    const std::size_t delay = std::size_t{kLead} * kBlock;
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        std::vector<float> expected(out[ch].size(), 0.0f), reference(out[ch].size() - delay);
        oracles[ch]->process(x.data(), reference.data(),
                             static_cast<std::uint32_t>(reference.size()));
        std::copy(reference.begin(), reference.end(), expected.begin() + delay);
        REQUIRE(max_abs_diff(out[ch], expected) <= kReferenceParity);
    }
    // The node's watchdog fences GPU delivery after capacity - 1 callback
    // blocks of provider silence (about 9 ms here). The paravirtual Metal
    // device in a macOS VM delivers, then falls that far behind under the
    // gate's load, so on a VM that one timing verdict is a property of the
    // host rather than of the GPU path; the blocks above prove the path ran
    // and the null check proves what it delivered was right.
    // On real hardware, and for every other recovery, the test still fails.
    if (node.epoch() == 0 && virtual_mac_host() &&
        node.recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive)
        SKIP("paravirtual Metal fell behind the provider watchdog after "
             << delivery.gpu_blocks << " GPU blocks (missed=" << status.missed_blocks
             << "); staying on the GPU needs a host that meets the block deadline");
    CHECK(delivery.gpu_blocks >= kBlocks / 2);
    REQUIRE(node.gpu_kernel_available());
    REQUIRE(node.epoch() > 0);
    REQUIRE(status.selected_engine == GpuAudioEngine::Gpu);
    REQUIRE(status.provider_state == GpuAudioProviderState::Ready);
    transport.release();
    REQUIRE(node.release());
}
#endif

// ---------------------------------------------------------------------------
// Family kernels. The references are independent scalar implementations with
// libm activations, written the way the WP-C test fixtures compute them.

namespace {

struct TcnRef {
    std::vector<float> taps, ring;
    float bias;
    std::uint32_t dilation, cursor = 0;
    TcnRef(TcnCpuShape s, std::vector<float> t, float b)
        : taps(std::move(t)), ring(tcn_cpu_receptive_field(s), 0.0f), bias(b),
          dilation(s.dilation) {}
    void process(const float* in, float* out, std::uint32_t n) {
        const auto size = static_cast<std::uint32_t>(ring.size());
        for (std::uint32_t i = 0; i < n; ++i) {
            ring[cursor] = in[i];
            float y = bias;
            for (std::uint32_t k = 0; k < taps.size(); ++k)
                y += taps[k] * ring[(cursor + size - k * dilation) % size];
            out[i] = std::tanh(y);
            cursor = (cursor + 1) % size;
        }
    }
};

struct NamTcnRef {
    std::uint32_t c, k, cursor = 0, ring_size = 0;
    std::vector<std::uint32_t> dilations;
    std::vector<float> in_w, conv_w, conv_b, mix_w, head_w, rings, act, x;
    NamTcnRef(std::uint32_t channels, std::uint32_t kernel, std::vector<std::uint32_t> d,
              std::uint32_t seed)
        : c(channels), k(kernel), dilations(std::move(d)) {
        const auto l = dilations.size();
        for (auto v : dilations)
            ring_size = std::max(ring_size, (k - 1) * v + 1);
        Rng w(seed);
        in_w.resize(c);
        conv_w.resize(l * c * c * k);
        conv_b.resize(l * c);
        mix_w.resize(l * c * c);
        head_w.resize(c);
        w.fill(in_w, 0.5f);
        w.fill(conv_w, 1.3f / std::sqrt(static_cast<float>(c * k)));
        w.fill(conv_b, 0.05f);
        w.fill(mix_w, 1.0f / std::sqrt(static_cast<float>(c)));
        w.fill(head_w, 0.5f);
        rings.assign(l * c * ring_size, 0.0f);
        act.assign(c, 0.0f);
        x.assign(c, 0.0f);
    }
    NamTcnSimdKernel<ns::DefaultIsa>::Weights weights() const {
        return {c, k, dilations, in_w, conv_w, conv_b, mix_w, head_w};
    }
    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            for (std::uint32_t ch = 0; ch < c; ++ch)
                x[ch] = in_w[ch] * in[i];
            for (std::size_t layer = 0; layer < dilations.size(); ++layer) {
                float* ring = rings.data() + layer * c * ring_size;
                for (std::uint32_t ch = 0; ch < c; ++ch)
                    ring[ch * ring_size + cursor] = x[ch];
                const auto d = dilations[layer];
                const float* wl = conv_w.data() + layer * c * c * k;
                for (std::uint32_t o = 0; o < c; ++o) {
                    float z = conv_b[layer * c + o];
                    for (std::uint32_t ch = 0; ch < c; ++ch) {
                        const float* r = ring + ch * ring_size;
                        const float* wk = wl + (static_cast<std::size_t>(o) * c + ch) * k;
                        for (std::uint32_t t = 0; t < k; ++t)
                            z += wk[t] * r[(cursor + ring_size - t * d) % ring_size];
                    }
                    act[o] = std::tanh(z);
                }
                const float* ml = mix_w.data() + layer * c * c;
                for (std::uint32_t o = 0; o < c; ++o) {
                    float r = 0.0f;
                    for (std::uint32_t ch = 0; ch < c; ++ch)
                        r += ml[o * c + ch] * act[ch];
                    x[o] += r;
                }
            }
            float y = 0.0f;
            for (std::uint32_t ch = 0; ch < c; ++ch)
                y += head_w[ch] * x[ch];
            out[i] = y;
            cursor = (cursor + 1) % ring_size;
        }
    }
};

float sigmoid_exact(float v) {
    return 1.0f / (1.0f + std::exp(-v));
}

struct RecurrentRef {
    bool lstm;
    std::uint32_t hidden;
    std::vector<float> w_ih, w_hh, bias, head, h, cell, gates, h_next;
    float head_bias = 0.0f;
    RecurrentRef(RecurrentFamily family, std::uint32_t hidden_size, std::uint32_t seed)
        : lstm(family == RecurrentFamily::Lstm), hidden(hidden_size) {
        const auto g = recurrent_gate_count(family);
        Rng w(seed);
        w_ih.resize(g * hidden);
        w_hh.resize(g * hidden * hidden);
        bias.resize(g * hidden);
        head.resize(hidden);
        w.fill(w_ih, 0.8f);
        w.fill(w_hh, 1.0f / std::sqrt(static_cast<float>(hidden)));
        w.fill(bias, 0.1f);
        w.fill(head, 1.0f / std::sqrt(static_cast<float>(hidden)));
        head_bias = w.uniform(0.05f);
        h.assign(hidden, 0.0f);
        cell.assign(hidden, 0.0f);
        gates.assign(g * hidden, 0.0f);
        h_next.assign(hidden, 0.0f);
    }
    RecurrentSimdKernel<ns::DefaultIsa>::Weights weights() const {
        return {w_ih, w_hh, bias, head, head_bias};
    }
    void process(const float* in, float* out, std::uint32_t n) {
        const auto rows = gates.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            const float x = in[i];
            for (std::size_t r = 0; r < rows; ++r) {
                float g = bias[r] + w_ih[r] * x;
                if (lstm || r < 2u * hidden) {
                    const float* wr = w_hh.data() + r * hidden;
                    for (std::uint32_t k = 0; k < hidden; ++k)
                        g += wr[k] * h[k];
                }
                gates[r] = g;
            }
            for (std::uint32_t k = 0; k < hidden; ++k) {
                if (lstm) {
                    const float ig = sigmoid_exact(gates[k]);
                    const float fg = sigmoid_exact(gates[hidden + k]);
                    const float cg = std::tanh(gates[2 * hidden + k]);
                    const float og = sigmoid_exact(gates[3 * hidden + k]);
                    cell[k] = fg * cell[k] + ig * cg;
                    h_next[k] = og * std::tanh(cell[k]);
                } else {
                    const float rg = sigmoid_exact(gates[k]);
                    const float zg = sigmoid_exact(gates[hidden + k]);
                    const float* wr = w_hh.data() + (2u * hidden + k) * hidden;
                    float recurrent = 0.0f;
                    for (std::uint32_t j = 0; j < hidden; ++j)
                        recurrent += wr[j] * h[j];
                    const float candidate = std::tanh(gates[2 * hidden + k] + rg * recurrent);
                    h_next[k] = (1.0f - zg) * candidate + zg * h[k];
                }
            }
            std::copy(h_next.begin(), h_next.end(), h.begin());
            float y = head_bias + x;
            for (std::uint32_t k = 0; k < hidden; ++k)
                y += head[k] * h[k];
            out[i] = y;
        }
    }
};

struct SsmRef {
    std::vector<float> a, b, c, h;
    float d;
    SsmRef(std::uint32_t states, std::uint32_t seed) : a(states), b(states), c(states), h(states) {
        Rng w(seed);
        for (auto& v : a)
            v = 0.5f + 0.49f * w.uniform(1.0f);
        w.fill(b, 0.5f);
        w.fill(c, 1.0f / std::sqrt(static_cast<float>(states)));
        d = w.uniform(0.3f);
    }
    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            float y = d * in[i];
            for (std::size_t s = 0; s < h.size(); ++s) {
                h[s] = a[s] * h[s] + b[s] * in[i];
                y += c[s] * h[s];
            }
            out[i] = y;
        }
    }
};

struct FamilyNull {
    double scalar_vs_simd = 0, reference_vs_simd = 0, partition = 0, peak = 0;
};

template <class Ref, class Scalar, class Simd>
FamilyNull family_null(const char* name, Ref& ref, Scalar& scalar, Simd& simd, std::uint32_t seed) {
    const auto x = stimulus(kNullNoise, seed, 4200);
    const auto bind = [](auto& k) {
        return [&k](const float* i, float* o, std::uint32_t n) { k.process(i, o, n); };
    };
    const auto ref_y = run_random_blocks(bind(ref), x, seed + 1);
    const auto scalar_y = run_random_blocks(bind(scalar), x, seed + 2);
    const auto simd_y = run_random_blocks(bind(simd), x, seed + 3);
    simd.reset();
    const auto simd_64 = run_fixed_blocks(bind(simd), x, 64);
    FamilyNull r;
    r.scalar_vs_simd = max_abs_diff(scalar_y, simd_y);
    r.reference_vs_simd = max_abs_diff(ref_y, simd_y);
    r.partition = max_abs_diff(simd_y, simd_64);
    for (float v : ref_y)
        r.peak = std::max(r.peak, static_cast<double>(std::abs(v)));
    std::printf("[family-null] %s isa=%s scalar_vs_simd=%.3g reference_vs_simd=%.3g "
                "partition=%.3g peak=%.3g samples=%zu\n",
                name, ns::DefaultIsa::name, r.scalar_vs_simd, r.reference_vs_simd, r.partition,
                r.peak, x.size());
    CHECK(r.peak > 1.0e-2);
    CHECK(r.scalar_vs_simd <= kSimdNull);
    CHECK(r.partition == 0.0);
    return r;
}

} // namespace

TEST_CASE("TCN SIMD kernel nulls scalar against SIMD and the reference",
          "[gpu_audio][neural][simd][tcn]") {
    for (const auto& [k, d] : {std::pair{64u, 64u}, std::pair{3u, 1u}, std::pair{7u, 5u}}) {
        const TcnCpuShape shape{.channels = 1, .kernel_size = k, .dilation = d, .layers = 1};
        Rng rng(k * 100 + d);
        std::vector<float> taps(k);
        rng.fill(taps, 1.5f / std::sqrt(static_cast<float>(k)));
        const float bias = rng.uniform(0.1f);
        TcnRef ref(shape, taps, bias);
        TcnSimdKernel<ns::ScalarIsa> scalar(shape, taps, bias);
        TcnSimdKernel<ns::DefaultIsa> simd(shape, taps, bias);
        const auto r = family_null("tcn", ref, scalar, simd, k + d);
        CHECK(r.reference_vs_simd <= kReferenceParity);
    }
}

TEST_CASE("NAM-TCN SIMD kernel nulls scalar against SIMD and the reference",
          "[gpu_audio][neural][simd][tcn]") {
    for (const std::uint32_t c : {8u, 16u, 5u}) {
        std::vector<std::uint32_t> dilations;
        for (std::uint32_t i = 0; i < 10; ++i)
            dilations.push_back(1u << i);
        NamTcnRef ref(c, 3, dilations, c);
        const auto w = ref.weights();
        NamTcnSimdKernel<ns::ScalarIsa> scalar(
            {w.channels, w.kernel, w.dilations, w.in_w, w.conv_w, w.conv_b, w.mix_w, w.head_w});
        NamTcnSimdKernel<ns::DefaultIsa> simd(w);
        const auto r = family_null("nam-tcn", ref, scalar, simd, c);
        CHECK(r.reference_vs_simd <= kReferenceParity * std::max(1.0, r.peak));
    }
}

TEST_CASE("LSTM and GRU SIMD kernels null scalar against SIMD and the reference",
          "[gpu_audio][neural][simd][recurrent]") {
    for (const auto family : {RecurrentFamily::Lstm, RecurrentFamily::Gru})
        for (const std::uint32_t hidden : kRecurrentHidden) {
            const RecurrentCpuShape shape{.family = family,
                                          .input_size = 1,
                                          .hidden_size = hidden,
                                          .layers = 1,
                                          .directions = 1,
                                          .output_size = 1};
            RecurrentRef ref(family, hidden, hidden + 3);
            const auto w = ref.weights();
            RecurrentSimdKernel<ns::ScalarIsa> scalar(
                shape, {w.w_ih, w.w_hh, w.bias, w.head, w.head_bias});
            RecurrentSimdKernel<ns::DefaultIsa> simd(shape, w);
            const auto r = family_null(family == RecurrentFamily::Lstm ? "lstm" : "gru", ref,
                                       scalar, simd, hidden);
            CHECK(r.reference_vs_simd <= kReferenceParity * std::max(1.0, r.peak));
        }
}

TEST_CASE("diagonal SSM SIMD kernel nulls scalar against SIMD and the reference",
          "[gpu_audio][neural][simd][ssm]") {
    for (const std::uint32_t states : {64u, 256u, 7u}) {
        SsmRef ref(states, states);
        SsmSimdKernel<ns::ScalarIsa> scalar(ref.a, ref.b, ref.c, ref.d);
        SsmSimdKernel<ns::DefaultIsa> simd(ref.a, ref.b, ref.c, ref.d);
        const auto r = family_null("ssm", ref, scalar, simd, states);
        CHECK(r.reference_vs_simd <= kReferenceParity * std::max(1.0, r.peak));
    }
}

namespace {

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

StreamingModelSpec family_spec(std::uint64_t state_bytes, std::string_view schema,
                               std::uint32_t receptive_field = 0) {
    return {.model_id = "pulp.simd",
            .architecture = "simd",
            .model_version = "1",
            .weights_hash = kSha,
            .runtime_hash = "pulp.neural.simd.v1",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 64,
            .feature_rate = 48000,
            .receptive_field_samples = receptive_field,
            .state_bytes = state_bytes,
            .state_schema = schema,
            .state_schema_version = state_bytes ? 1u : 0u};
}

/// Drives an adapter for many blocks under the RT probe and compares it with
/// a second, directly driven kernel instance.
template <class Adapter, class Kernel>
void adapter_matches_direct(Adapter& adapter, Kernel& direct) {
    REQUIRE(adapter.prepare({.spec = &adapter.spec(),
                             .artifact_id = "simd.weights",
                             .artifact_hash = kSha,
                             .max_frames = 256}));
    const auto x = stimulus(6000, 7, 300);
    std::vector<float> y(x.size()), expected(x.size());
    for (std::size_t at = 0; at + 128 <= x.size(); at += 128) {
        const float* in_ptr[] = {x.data() + at};
        float* out_ptr[] = {y.data() + at};
        const pulp::audio::BufferView<const float> in(in_ptr, 1, 128);
        pulp::audio::BufferView<float> out(out_ptr, 1, 128);
        pulp::test::ScopedRtProcessProbe probe;
        adapter.process_cpu(in, out, 128, {.epoch = 1, .sequence = at / 128});
        REQUIRE(probe.allocation_count() == 0);
        direct.process(x.data() + at, expected.data() + at, 128);
    }
    CHECK(max_abs_diff(y, expected) == 0.0);
    double peak = 0.0;
    for (float v : y)
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    CHECK(peak > 1.0e-2);
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
}

} // namespace

TEST_CASE("SIMD family kernels run behind the private adapters without allocating",
          "[gpu_audio][neural][simd][adapter]") {
    SECTION("TCN") {
        const TcnCpuShape shape{.channels = 1, .kernel_size = 64, .dilation = 64, .layers = 1};
        std::vector<float> taps(64);
        Rng(5).fill(taps, 0.2f);
        TcnSimdKernel<> kernel(shape, taps, 0.01f), direct(shape, taps, 0.01f);
        TcnCpuAdapter adapter(family_spec(tcn_cpu_state_bytes(shape), "causal-tcn-v1",
                                          tcn_cpu_receptive_field(shape)),
                              shape,
                              {.artifact_id = "simd.weights",
                               .artifact_sha256 = kSha,
                               .license_id = "MIT",
                               .tensor_schema = "causal-tcn-v1",
                               .tensor_schema_version = 1},
                              bind_neural_cpu_kernel<TcnCpuKernel>(kernel));
        adapter_matches_direct(adapter, direct);
    }
    SECTION("NAM-TCN") {
        NamTcnRef ref(16, 3, {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}, 9);
        NamTcnSimdKernel<> kernel(ref.weights()), direct(ref.weights());
        NamTcnStreamingAdapter adapter(family_spec(0, ""),
                                       bind_neural_cpu_kernel<NamTcnCpuKernel>(kernel));
        adapter_matches_direct(adapter, direct);
    }
    SECTION("LSTM and GRU") {
        for (const auto family : {RecurrentFamily::Lstm, RecurrentFamily::Gru}) {
            const RecurrentCpuShape shape{.family = family,
                                          .input_size = 1,
                                          .hidden_size = 32,
                                          .layers = 1,
                                          .directions = 1,
                                          .output_size = 1};
            RecurrentRef ref(family, 32, 17);
            RecurrentSimdKernel<> kernel(shape, ref.weights()), direct(shape, ref.weights());
            RecurrentCpuAdapter adapter(
                family_spec(recurrent_state_bytes(shape), recurrent_state_schema(family)), shape,
                bind_neural_cpu_kernel<RecurrentCpuKernel>(kernel));
            adapter_matches_direct(adapter, direct);
        }
    }
    SECTION("SSM") {
        const CompactSsmCpuShape shape{
            .state_size = 256, .input_size = 1, .output_size = 1, .layers = 1};
        SsmRef ref(256, 3);
        SsmSimdKernel<> kernel(ref.a, ref.b, ref.c, ref.d), direct(ref.a, ref.b, ref.c, ref.d);
        CompactSsmCpuAdapter adapter(
            family_spec(compact_ssm_cpu_state_bytes(shape), "compact-ssm-diagonal-v1"), shape,
            {.artifact_id = "simd.weights",
             .artifact_sha256 = kSha,
             .license_id = "MIT",
             .tensor_schema = "compact-ssm-diagonal-v1",
             .tensor_schema_version = 1},
            bind_neural_cpu_kernel<CompactSsmCpuKernel>(kernel));
        adapter_matches_direct(adapter, direct);
    }
}

TEST_CASE("SIMD family kernels fail closed on inconsistent construction",
          "[gpu_audio][neural][simd][adapter]") {
    const StreamingPrepareContext unused{};
    std::array<float, 8> in{0.5f, -0.25f, 0.1f, 0.0f, 0.3f, -0.7f, 0.2f, 0.9f};
    const auto silent = [&](auto& kernel) {
        std::array<float, 8> out;
        out.fill(7.0f);
        kernel.process(in.data(), out.data(), static_cast<std::uint32_t>(out.size()));
        return std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; });
    };
    SECTION("TCN: tap count must equal kernel_size") {
        const TcnCpuShape shape{.channels = 1, .kernel_size = 4, .dilation = 2, .layers = 1};
        const std::vector<float> five(5, 0.1f), four(4, 0.1f);
        TcnSimdKernel<> bad(shape, five, 0.0f), good(shape, four, 0.0f);
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(bad.prepare(unused));
        CHECK(silent(bad));
        CHECK(good.valid());
        CHECK(good.prepare(unused));
        TcnSimdKernel<> bad_shape({.channels = 2, .kernel_size = 4, .dilation = 2, .layers = 1},
                                  four, 0.0f);
        CHECK_FALSE(bad_shape.valid());
    }
    SECTION("NAM-TCN: dilations non-empty and spans long enough") {
        NamTcnRef ref(4, 3, {1, 2}, 3);
        auto w = ref.weights();
        NamTcnSimdKernel<> good(w);
        CHECK(good.valid());
        CHECK_FALSE(silent(good));
        auto no_dilations = w;
        no_dilations.dilations = {};
        auto short_conv = w;
        short_conv.conv_w = short_conv.conv_w.first(short_conv.conv_w.size() - 1);
        auto short_head = w;
        short_head.head_w = short_head.head_w.first(2);
        const std::array<std::uint32_t, 2> zero_dilation{1, 0};
        auto bad_dilation = w;
        bad_dilation.dilations = zero_dilation;
        for (const auto& bad_weights : {no_dilations, short_conv, short_head, bad_dilation}) {
            NamTcnSimdKernel<> bad(bad_weights);
            CHECK_FALSE(bad.valid());
            CHECK_FALSE(bad.prepare(unused));
            CHECK(silent(bad));
        }
    }
    SECTION("recurrent: shape and spans") {
        const RecurrentCpuShape shape{.family = RecurrentFamily::Lstm,
                                      .input_size = 1,
                                      .hidden_size = 8,
                                      .layers = 1,
                                      .directions = 1,
                                      .output_size = 1};
        RecurrentRef ref(RecurrentFamily::Lstm, 8, 3);
        auto w = ref.weights();
        RecurrentSimdKernel<> good(shape, w);
        CHECK(good.valid());
        auto short_hh = w;
        short_hh.w_hh = short_hh.w_hh.first(short_hh.w_hh.size() - 1);
        RecurrentSimdKernel<> bad(shape, short_hh);
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(bad.prepare(unused));
        CHECK(silent(bad));
        auto two_layers = shape;
        two_layers.layers = 2;
        RecurrentSimdKernel<> bad_shape(two_layers, w);
        CHECK_FALSE(bad_shape.valid());
        // The adapter refuses to prepare over a kernel that failed closed.
        RecurrentCpuAdapter adapter(
            family_spec(recurrent_state_bytes(shape), recurrent_state_schema(shape.family)), shape,
            bind_neural_cpu_kernel<RecurrentCpuKernel>(bad));
        CHECK_FALSE(adapter.prepare({.spec = &adapter.spec(),
                                     .artifact_id = "simd.weights",
                                     .artifact_hash = kSha,
                                     .max_frames = 256}));
    }
    SECTION("NAM-TCN: every weight span must be exactly the shape's size") {
        NamTcnRef ref(4, 3, {1, 2}, 3);
        const auto w = ref.weights();
        // Backing storage longer than any span, so a too-long span is a valid
        // view and only the kernel's own copies could run past its buffers.
        std::vector<float> spare(4096, 0.25f);
        const auto longer = [&](std::span<const float> exact, std::size_t extra) {
            std::copy(exact.begin(), exact.end(), spare.begin());
            return std::span<const float>(spare.data(), exact.size() + extra);
        };
        const auto shorter = [](std::span<const float> exact) {
            return exact.first(exact.size() - 1);
        };
        using Field = std::span<const float> NamTcnSimdKernel<>::Weights::*;
        for (const Field field :
             {&NamTcnSimdKernel<>::Weights::in_w, &NamTcnSimdKernel<>::Weights::conv_w,
              &NamTcnSimdKernel<>::Weights::conv_b, &NamTcnSimdKernel<>::Weights::mix_w,
              &NamTcnSimdKernel<>::Weights::head_w}) {
            // 64 extra floats exceed every padded buffer the kernel packs
            // into, so a size check that tolerated a long span would overflow.
            auto too_long = w;
            too_long.*field = longer(w.*field, 64);
            auto too_short = w;
            too_short.*field = shorter(w.*field);
            for (const auto& bad_weights : {too_long, too_short}) {
                NamTcnSimdKernel<> bad(bad_weights);
                CHECK_FALSE(bad.valid());
                CHECK_FALSE(bad.prepare(unused));
                CHECK(silent(bad));
            }
        }
        NamTcnSimdKernel<> exact(w);
        CHECK(exact.valid());
        CHECK_FALSE(silent(exact));
    }
    SECTION("recurrent: every weight span must be exactly the shape's size") {
        for (const auto family : {RecurrentFamily::Lstm, RecurrentFamily::Gru}) {
            const RecurrentCpuShape shape{.family = family,
                                          .input_size = 1,
                                          .hidden_size = 8,
                                          .layers = 1,
                                          .directions = 1,
                                          .output_size = 1};
            RecurrentRef ref(family, 8, 3);
            const auto w = ref.weights();
            std::vector<float> spare(8192, 0.25f);
            using Field = std::span<const float> RecurrentSimdKernel<>::Weights::*;
            for (const Field field :
                 {&RecurrentSimdKernel<>::Weights::w_ih, &RecurrentSimdKernel<>::Weights::w_hh,
                  &RecurrentSimdKernel<>::Weights::bias, &RecurrentSimdKernel<>::Weights::head}) {
                const auto exact = w.*field;
                std::copy(exact.begin(), exact.end(), spare.begin());
                auto too_long = w;
                too_long.*field = std::span<const float>(spare.data(), exact.size() + 64);
                auto too_short = w;
                too_short.*field = exact.first(exact.size() - 1);
                for (const auto& bad_weights : {too_long, too_short}) {
                    RecurrentSimdKernel<> bad(shape, bad_weights);
                    CHECK_FALSE(bad.valid());
                    CHECK_FALSE(bad.prepare(unused));
                    CHECK(silent(bad));
                }
            }
            RecurrentSimdKernel<> exact(shape, w);
            CHECK(exact.valid());
            CHECK_FALSE(silent(exact));
        }
    }
    SECTION("TCN and SSM: a too-long span is refused too") {
        const TcnCpuShape shape{.channels = 1, .kernel_size = 4, .dilation = 2, .layers = 1};
        const std::vector<float> taps(4 + 64, 0.1f);
        TcnSimdKernel<> tcn(shape, taps, 0.0f);
        CHECK_FALSE(tcn.valid());
        SsmRef ref(16, 3);
        std::vector<float> long_b(ref.b);
        long_b.resize(16 + 64, 0.1f);
        SsmSimdKernel<> ssm(ref.a, long_b, ref.c, ref.d);
        CHECK_FALSE(ssm.valid());
    }
    SECTION("SSM: matching non-empty spans") {
        SsmRef ref(16, 3);
        SsmSimdKernel<> good(ref.a, ref.b, ref.c, ref.d);
        CHECK(good.valid());
        const std::span<const float> short_c(ref.c.data(), 15);
        SsmSimdKernel<> bad(ref.a, ref.b, short_c, ref.d);
        CHECK_FALSE(bad.valid());
        CHECK(silent(bad));
        SsmSimdKernel<> empty({}, {}, {}, 0.0f);
        CHECK_FALSE(empty.valid());
    }
}

namespace {

/// A decaying diagonal SSM: an impulse then silence halves the state each
/// sample, so it passes through the subnormal range after ~126 samples.
std::size_t subnormal_outputs(bool flush) {
    constexpr std::uint32_t states = 16, frames = 1024;
    const std::vector<float> a(states, 0.5f), b(states, 1.0f), c(states, 1.0f / states);
    SsmSimdKernel<> ssm(a, b, c, 0.0f);
    REQUIRE(ssm.valid());
    std::vector<float> x(frames, 0.0f), y(frames, 0.0f);
    x[0] = 1.0f;
    {
        std::optional<pulp::signal::ScopedFlushDenormals> scope;
        if (flush)
            scope.emplace();
        ssm.process(x.data(), y.data(), frames);
    }
    return static_cast<std::size_t>(std::count_if(
        y.begin(), y.end(), [](float v) { return std::fpclassify(v) == FP_SUBNORMAL; }));
}

} // namespace

TEST_CASE("SIMD kernels produce no subnormals under the realtime flush scope",
          "[gpu_audio][neural][simd][denormal]") {
    // Positive control: without flush-to-zero the decay really is subnormal,
    // so the check below can see a missing scope.
    const auto unflushed = subnormal_outputs(false);
    CHECK(unflushed > 0);
    CHECK(subnormal_outputs(true) == 0);
    std::printf("[denormal] subnormal outputs: unflushed=%zu flushed=0\n", unflushed);
}

TEST_CASE("neural SIMD denormal-tail cost", "[.][neural-simd-denormal]") {
    // Receipt, not a gate: per-block cost on a subnormal decay tail compared
    // with ordinary input, with and without the flush scope.
    constexpr std::uint32_t states = 256, block = 64;
    const std::vector<float> a(states, 0.5f), b(states, 1.0f), c(states, 1.0f / states);
    const auto run = [&](bool denormal_tail, bool flush) {
        SsmSimdKernel<> ssm(a, b, c, 0.0f);
        std::optional<pulp::signal::ScopedFlushDenormals> scope;
        if (flush)
            scope.emplace();
        std::vector<float> x(block), y(block);
        Rng rng(3);
        std::vector<double> samples;
        for (int i = 0; i < 3300; ++i) {
            // Keep the state in the subnormal range: re-seed with a tiny
            // impulse each block and let it decay.
            for (auto& v : x)
                v = denormal_tail ? 0.0f : rng.uniform(1.0f);
            if (denormal_tail)
                x[0] = 1.0e-36f;
            const auto t0 = std::chrono::steady_clock::now();
            ssm.process(x.data(), y.data(), block);
            const auto t1 = std::chrono::steady_clock::now();
            if (i >= 300)
                samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        return std::pair{samples[samples.size() / 2], samples[samples.size() * 99 / 100]};
    };
    for (const bool flush : {false, true})
        for (const bool tail : {false, true}) {
            const auto [p50, p99] = run(tail, flush);
            std::printf("{\"kernel\":\"ssm.diagonal(256)\",\"input\":\"%s\",\"flush\":%s,"
                        "\"p50_us\":%.2f,\"p99_us\":%.2f}\n",
                        tail ? "denormal-tail" : "noise", flush ? "true" : "false", p50, p99);
        }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Perf receipt (measurement, not a gate). Run explicitly:
//   pulp-test-neural-cpu-simd "[neural-simd-perf]"
// Per-block wall time for one channel at 48 kHz, after a warm-up.

namespace {

struct Timing {
    double p50_us, p99_us;
};

Timing time_blocks(const std::function<void(const float*, float*, std::uint32_t)>& process,
                   std::uint32_t block) {
    constexpr int warmup = 300, measured = 4000;
    const auto x = stimulus(static_cast<std::size_t>(block) * 64, 3, 64);
    std::vector<float> y(block);
    std::vector<double> samples;
    samples.reserve(measured);
    std::size_t at = 0;
    for (int i = 0; i < warmup + measured; ++i) {
        const float* in = x.data() + at;
        at = (at + block) % (x.size() - block);
        const auto start = std::chrono::steady_clock::now();
        process(in, y.data(), block);
        const auto stop = std::chrono::steady_clock::now();
        if (i >= warmup)
            samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[samples.size() * 99 / 100]};
}

void report(const char* family, const char* variant, std::uint32_t block, Timing t) {
    const double period = 1e6 * block / 48000.0;
    std::printf("{\"family\":\"%s\",\"kernel\":\"%s\",\"block\":%u,\"p50_us\":%.2f,"
                "\"p99_us\":%.2f,\"p50_pct\":%.2f,\"p99_pct\":%.2f}\n",
                family, variant, block, t.p50_us, t.p99_us, 100.0 * t.p50_us / period,
                100.0 * t.p99_us / period);
}

template <class K> std::function<void(const float*, float*, std::uint32_t)> bind(K& k) {
    return [&k](const float* i, float* o, std::uint32_t n) { k.process(i, o, n); };
}

} // namespace

TEST_CASE("neural SIMD per-block CPU cost", "[.][neural-simd-perf]") {
    std::printf("\nneural CPU kernels, 1 channel, 48 kHz, isa=%s\n", ns::DefaultIsa::name);
    std::vector<std::pair<std::string, std::string>> nam = {
        {"wavenet.nam-standard(16/8ch,2x10)", write_nam("perf-standard", nam_standard(), 7)},
        {"wavenet.nam-lite(12/6ch,2x10)", write_nam("perf-lite", nam_lite(), 7)},
        {"wavenet.nam-nano(4/2ch,2x10)", write_nam("perf-nano", nam_nano(), 7)}};
    for (const auto& path : extra_models())
        nam.emplace_back("wavenet." + std::filesystem::path(path).stem().string(), path);
    for (const std::uint32_t block : {64u, 128u}) {
        for (const auto& [name, path] : nam)
            for (const auto kernel :
                 {NamCpuKernel::Reference, NamCpuKernel::BlockScalar, NamCpuKernel::BlockSimd}) {
                auto a = load_nam(path, kernel);
                report(name.c_str(),
                       kernel == NamCpuKernel::Reference     ? "reference"
                       : kernel == NamCpuKernel::BlockScalar ? "scalar"
                                                             : "simd",
                       block, time_blocks(bind(*a), block));
            }

        const TcnCpuShape tcn{.channels = 1, .kernel_size = 64, .dilation = 64, .layers = 1};
        std::vector<float> taps(64);
        Rng(1).fill(taps, 0.2f);
        TcnRef tcn_ref(tcn, taps, 0.01f);
        TcnSimdKernel<ns::ScalarIsa> tcn_scalar(tcn, taps, 0.01f);
        TcnSimdKernel<> tcn_simd(tcn, taps, 0.01f);
        report("tcn.scalar(k64,d64)", "reference", block, time_blocks(bind(tcn_ref), block));
        report("tcn.scalar(k64,d64)", "scalar", block, time_blocks(bind(tcn_scalar), block));
        report("tcn.scalar(k64,d64)", "simd", block, time_blocks(bind(tcn_simd), block));

        for (const std::uint32_t c : {8u, 16u}) {
            NamTcnRef ref(c, 3, {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}, 7);
            const auto w = ref.weights();
            NamTcnSimdKernel<ns::ScalarIsa> scalar(
                {w.channels, w.kernel, w.dilations, w.in_w, w.conv_w, w.conv_b, w.mix_w, w.head_w});
            NamTcnSimdKernel<> simd(w);
            const auto name = "nam-tcn(" + std::to_string(c) + "ch,k3,10L)";
            report(name.c_str(), "reference", block, time_blocks(bind(ref), block));
            report(name.c_str(), "scalar", block, time_blocks(bind(scalar), block));
            report(name.c_str(), "simd", block, time_blocks(bind(simd), block));
        }

        for (const auto family : {RecurrentFamily::Lstm, RecurrentFamily::Gru})
            for (const std::uint32_t hidden : {24u, 64u}) {
                const RecurrentCpuShape shape{.family = family,
                                              .input_size = 1,
                                              .hidden_size = hidden,
                                              .layers = 1,
                                              .directions = 1,
                                              .output_size = 1};
                RecurrentRef ref(family, hidden, 7);
                const auto w = ref.weights();
                RecurrentSimdKernel<ns::ScalarIsa> scalar(
                    shape, {w.w_ih, w.w_hh, w.bias, w.head, w.head_bias});
                RecurrentSimdKernel<> simd(shape, w);
                const auto name =
                    std::string(family == RecurrentFamily::Lstm ? "rnn.lstm(" : "rnn.gru(") +
                    std::to_string(hidden) + ")";
                report(name.c_str(), "reference", block, time_blocks(bind(ref), block));
                report(name.c_str(), "scalar", block, time_blocks(bind(scalar), block));
                report(name.c_str(), "simd", block, time_blocks(bind(simd), block));
            }

        for (const std::uint32_t states : {64u, 256u}) {
            SsmRef ref(states, 7);
            SsmSimdKernel<ns::ScalarIsa> scalar(ref.a, ref.b, ref.c, ref.d);
            SsmSimdKernel<> simd(ref.a, ref.b, ref.c, ref.d);
            const auto name = "ssm.diagonal(" + std::to_string(states) + ")";
            report(name.c_str(), "reference", block, time_blocks(bind(ref), block));
            report(name.c_str(), "scalar", block, time_blocks(bind(scalar), block));
            report(name.c_str(), "simd", block, time_blocks(bind(simd), block));
        }
    }
    SUCCEED();
}
