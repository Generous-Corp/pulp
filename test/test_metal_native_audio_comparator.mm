// Matched Dawn-vs-native-Metal comparator for the shared-I/O convolution
// program (WP-Q decision gate). Test-only; never runs from an audio callback.
//
// Both sides run the production shared-I/O convolution program shape: log2(N)
// radix-2 Stockham forward passes, one complex multiply against a prepared IR
// spectrum, and log2(N) inverse passes, each pass its own compute encoder.
// The Dawn side compiles the WGSL inside DawnSharedIoProvider; the Metal side
// compiles an MSL port of that WGSL. The port records the SHA-256 of the WGSL
// it was written against, and the comparator extracts the WGSL actually built
// into the provider from its source, so a stale port or a different kernel
// produces a different kernel digest and the verifier refuses the receipt.
//
// Workload: 64/128/256 frames at 48 kHz, two channels, two-slot flight depth,
// page-aligned host memory (imported on Dawn, no-copy on Metal), prebuilt
// pipelines and buffers. Each cell is paced at the block period, idle or under
// a concurrent Skia render load, and emits raw per-block samples into a
// pulp.gpu-audio.comparator.v2 receipt. Statistics and the PROPOSED gate
// thresholds are evaluated only by tools/scripts/gpu_audio_comparator_v2.py.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>

#if __has_include("detail/dawn_shared_io_provider.hpp")
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_compute_plan.hpp"
#define PULP_HAS_DAWN_COMPARATOR 1
#else
#define PULP_HAS_DAWN_COMPARATOR 0
#endif

#if defined(PULP_COMPARATOR_HAS_RENDER_LOAD)
#include <pulp/canvas/canvas.hpp>
#include <pulp/render/headless_surface.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fstream>
#include <functional>
#include <future>
#include <mach/mach_time.h>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/qos.h>
#include <sys/resource.h>
#include <thread>
#include <tuple>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
#ifndef PULP_GPU_AUDIO_PROVIDER_SOURCE
#define PULP_GPU_AUDIO_PROVIDER_SOURCE ""
#endif
// The provider WGSL hashes taken from its source at configure time, i.e. the
// WGSL compiled into this binary. The run-time extraction must agree.
#ifndef PULP_COMPARATOR_BUILD_FFT_WGSL_SHA256
#define PULP_COMPARATOR_BUILD_FFT_WGSL_SHA256 ""
#endif
#ifndef PULP_COMPARATOR_BUILD_MUL_WGSL_SHA256
#define PULP_COMPARATOR_BUILD_MUL_WGSL_SHA256 ""
#endif

namespace {

constexpr const char* kSchema = "pulp.gpu-audio.comparator.v2";
constexpr const char* kProgramId = "pulp.shared_io_convolution.v1";
constexpr const char* kPlantedProgramId = "pulp.native_metal.complex_multiply.v0";
// SHA-256 of the provider WGSL this MSL port was written against.
constexpr const char* kPortedFromFftWgslSha256 =
    "8275969f135a4024fb78bf0a9efe1795ac55b252803aaad7b3321b7aa0b35faa";
constexpr const char* kPortedFromMulWgslSha256 =
    "988d833cb4431aff69a0c43ab5a60e076e7c987f8bc26ee3ff3400a1b56da568";
// SHA-256 of kMatchedMsl, the reviewed port pinned to that WGSL. Editing the
// MSL without re-pinning this hash is a kernel-digest mismatch.
constexpr const char* kPinnedMslSha256 =
    "e71759de1adb3966ab80e658997ae6c56f78666030ac67f7f64e4229f3b2aa63";
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kFlightDepth = 2;
constexpr std::uint32_t kLeadBlocks = 2;
constexpr std::uint32_t kPatterns = 8;
constexpr std::uint32_t kWorkgroup = 256;
constexpr float kOracleTolerance = 1.0e-3f;
constexpr double kParityTolerance = 1.0e-4;
constexpr std::uint64_t kDawnPollIntervalNs = 50'000;
constexpr std::uint64_t kStallLimitNs = 1'000'000'000;

// ---------------------------------------------------------------------------
// Clocks, hashing, JSON

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t thread_cpu_ns() {
    return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}

std::uint64_t process_cpu_ns() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto tv = [](const timeval& t) {
        return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000ull +
               static_cast<std::uint64_t>(t.tv_usec) * 1'000ull;
    };
    return tv(usage.ru_utime) + tv(usage.ru_stime);
}

void sleep_until_ns(std::uint64_t deadline) {
    const auto now = now_ns();
    if (deadline > now)
        std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
}

// Metal reports GPUStartTime/GPUEndTime in mach-absolute seconds; steady_clock
// on Darwin is also mach-absolute based. The measured offset is recorded.
struct HostClockBridge {
    double ticks_to_ns = 1.0;
    std::int64_t steady_minus_mach_ns = 0;
    HostClockBridge() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        ticks_to_ns = static_cast<double>(info.numer) / static_cast<double>(info.denom);
        std::uint64_t best_span = UINT64_MAX;
        for (int i = 0; i < 64; ++i) {
            const auto a = now_ns();
            const auto mach = static_cast<std::uint64_t>(mach_absolute_time() * ticks_to_ns);
            const auto b = now_ns();
            if (b - a < best_span) {
                best_span = b - a;
                steady_minus_mach_ns =
                    static_cast<std::int64_t>(a + (b - a) / 2) - static_cast<std::int64_t>(mach);
            }
        }
    }
    std::int64_t from_metal_seconds(double seconds) const {
        return static_cast<std::int64_t>(seconds * 1.0e9) + steady_minus_mach_ns;
    }
};

std::string sha256_hex(const void* data, std::size_t size) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, static_cast<CC_LONG>(size), digest);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 15]);
    }
    return out;
}

std::string sha256_hex(const std::string& text) {
    return sha256_hex(text.data(), text.size());
}

std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out += "\"";
    return out;
}

template <class T> std::string json_array(const std::vector<T>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i)
            out += ",";
        out += std::to_string(values[i]);
    }
    out += "]";
    return out;
}

std::string json_double(double value) {
    if (!std::isfinite(value))
        return "null";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

const char* json_bool(bool value) {
    return value ? "true" : "false";
}

// ---------------------------------------------------------------------------
// Kernel identity

struct WgslExtraction {
    bool ok = false;
    std::string source_path;
    std::string source_sha256;
    std::string fft_sha256;
    std::string mul_sha256;
    std::string reason;
};

std::optional<std::string> extract_raw_string(const std::string& text, const std::string& name) {
    const auto marker = "constexpr auto " + name + " = R\"wgsl(";
    const auto begin = text.find(marker);
    if (begin == std::string::npos)
        return std::nullopt;
    const auto body = begin + marker.size();
    const auto end = text.find(")wgsl\"", body);
    if (end == std::string::npos)
        return std::nullopt;
    return text.substr(body, end - body);
}

// Reads the WGSL that DawnSharedIoProvider compiles, from the source the
// provider library was built from.
WgslExtraction extract_provider_wgsl(const std::string& path) {
    WgslExtraction out;
    out.source_path = path;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out.reason = "provider_source_unreadable";
        return out;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto text = buffer.str();
    out.source_sha256 = sha256_hex(text);
    const auto fft = extract_raw_string(text, "kSharedIoFftWgsl");
    const auto mul = extract_raw_string(text, "kSharedIoMulWgsl");
    if (!fft || !mul) {
        out.reason = "provider_wgsl_not_found";
        return out;
    }
    out.fft_sha256 = sha256_hex(*fft);
    out.mul_sha256 = sha256_hex(*mul);
    out.ok = true;
    return out;
}

struct PassShape {
    std::string kernel;
    int sign = 0;
    std::uint32_t ns = 0;
    std::uint32_t groups = 0;
};

struct Workload {
    std::uint32_t frames = 0;
    std::uint32_t fft_size = 0;
    std::uint32_t ir_length = 0;
    std::uint32_t log2 = 0;
    std::size_t complex_floats = 0;                     // kChannels * fft_size * 2
    std::vector<float> ir_spectrum;                     // normalized by 1/N, interleaved complex
    std::array<std::vector<float>, kPatterns> inputs;   // interleaved complex
    std::array<std::vector<float>, kPatterns> expected; // real, kChannels * fft_size
    double period_ns = 0.0;
};

// The provider's convolution submit contract: log2(N) forward passes, one
// multiply, log2(N) inverse passes; FFT passes cover channels*N/2 butterflies
// and the multiply channels*N bins, in 256-wide workgroups.
std::vector<PassShape> convolution_passes(const Workload& w) {
    std::vector<PassShape> passes;
    const auto fft_groups = (kChannels * (w.fft_size / 2u) + kWorkgroup - 1) / kWorkgroup;
    for (std::uint32_t stage = 0; stage < w.log2; ++stage)
        passes.push_back({"stockham_fft", -1, 1u << stage, fft_groups});
    passes.push_back(
        {"complex_multiply_ir", 0, 0, (kChannels * w.fft_size + kWorkgroup - 1) / kWorkgroup});
    for (std::uint32_t stage = 0; stage < w.log2; ++stage)
        passes.push_back({"stockham_fft", 1, 1u << stage, fft_groups});
    return passes;
}

struct KernelIdentity {
    std::string canonical;
    std::string digest;
};

// Robustness of the compiled kernels. Dawn always compiles with Tint's
// robustness and integer polyfills; the gating Metal lanes run the guarded
// MSL that reproduces them, and only the labelled robustness-cost probe runs
// without guards.
constexpr const char* kRobustnessTint = "tint_equivalent";
constexpr const char* kRobustnessNone = "none";

// Canonical form matches json.dumps(sort_keys=True, separators=(",", ":")), so
// the verifier recomputes the digest instead of trusting it. `port_sha` names
// the MSL port: the compiled source on Metal, and the port pinned to this WGSL
// on Dawn, so an edited port without a re-pinned hash is a digest mismatch.
KernelIdentity make_kernel_identity(const std::string& program, const std::string& fft_sha,
                                    const std::string& mul_sha, const std::string& port_sha,
                                    const char* robustness, const Workload& w,
                                    const std::vector<PassShape>& passes) {
    std::string c = "{\"channels\":" + std::to_string(kChannels) +
                    ",\"fft_size\":" + std::to_string(w.fft_size) +
                    ",\"fft_wgsl_sha256\":" + json_string(fft_sha) +
                    ",\"frames\":" + std::to_string(w.frames) +
                    ",\"ir_length\":" + std::to_string(w.ir_length) +
                    ",\"metal_port_sha256\":" + json_string(port_sha) +
                    ",\"mul_wgsl_sha256\":" + json_string(mul_sha) + ",\"passes\":[";
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto& p = passes[i];
        if (i)
            c += ",";
        c += "{\"groups\":" + std::to_string(p.groups) + ",\"kernel\":" + json_string(p.kernel) +
             ",\"ns\":" + std::to_string(p.ns) + ",\"sign\":" + std::to_string(p.sign) +
             ",\"workgroup\":" + std::to_string(kWorkgroup) + "}";
    }
    c += "],\"program\":" + json_string(program) + ",\"robustness\":" + json_string(robustness) +
         "}";
    return {c, sha256_hex(c)};
}

// MSL port of kSharedIoFftWgsl and kSharedIoMulWgsl (hashes above).
//
// With kGuarded (function constant 0) the kernels carry the guards Tint adds
// when Dawn compiles the WGSL for Metal: every storage-buffer index is clamped
// to the bound length, and unsigned `/` and `%` substitute 1 for a zero
// divisor. This is an equivalent of Tint's robustness transform and integer
// polyfills, not Tint's literal output. kGuarded=false is the robustness-cost
// probe only.
constexpr const char* kMatchedMsl = R"msl(#include <metal_stdlib>
using namespace metal;

constant bool kGuarded [[function_constant(0)]];

static inline uint guard_index(uint i, uint len) { return kGuarded ? min(i, len - 1u) : i; }
static inline uint guard_div(uint a, uint b) { return kGuarded ? a / select(b, 1u, b == 0u) : a / b; }
static inline uint guard_mod(uint a, uint b) { return kGuarded ? a % select(b, 1u, b == 0u) : a % b; }

struct FftParams { uint n; uint ns; float sign; uint batch; };
struct FftLengths { uint src_len; uint dst_len; };
constant float PI = 3.1415926535897932f;

kernel void stockham_fft(const device float* src [[buffer(0)]],
                         device float* dst [[buffer(1)]],
                         constant FftParams& params [[buffer(2)]],
                         constant FftLengths& lengths [[buffer(3)]],
                         uint3 gid [[thread_position_in_grid]]) {
    const uint half_n = guard_div(params.n, 2u);
    const uint tid = gid.x;
    if (tid >= params.batch * half_n) {
        return;
    }
    const uint b = guard_div(tid, half_n);
    const uint j = tid - b * half_n;
    const uint base = b * params.n;
    const uint sl = lengths.src_len;
    const uint dl = lengths.dst_len;

    const float v0_re = src[guard_index(2u * (base + j), sl)];
    const float v0_im = src[guard_index(2u * (base + j) + 1u, sl)];
    const uint k = j + half_n;
    const float v1_re = src[guard_index(2u * (base + k), sl)];
    const float v1_im = src[guard_index(2u * (base + k) + 1u, sl)];

    const float angle = params.sign * 2.0f * PI * float(guard_mod(j, params.ns)) /
                        float(params.ns * 2u);
    const float tw_re = cos(angle);
    const float tw_im = sin(angle);

    const float u1_re = tw_re * v1_re - tw_im * v1_im;
    const float u1_im = tw_re * v1_im + tw_im * v1_re;

    const float y0_re = v0_re + u1_re;
    const float y0_im = v0_im + u1_im;
    const float y1_re = v0_re - u1_re;
    const float y1_im = v0_im - u1_im;

    const uint idxD = guard_div(j, params.ns) * params.ns * 2u + guard_mod(j, params.ns);
    dst[guard_index(2u * (base + idxD), dl)] = y0_re;
    dst[guard_index(2u * (base + idxD) + 1u, dl)] = y0_im;
    dst[guard_index(2u * (base + idxD + params.ns), dl)] = y1_re;
    dst[guard_index(2u * (base + idxD + params.ns) + 1u, dl)] = y1_im;
}

struct MulParams { uint a_len; uint ir_len; uint out_len; };

kernel void complex_multiply_ir(const device float* a [[buffer(0)]],
                                const device float* ir [[buffer(1)]],
                                device float* out [[buffer(2)]],
                                constant MulParams& lengths [[buffer(3)]],
                                uint3 gid [[thread_position_in_grid]]) {
    const uint i = gid.x;
    const uint p = i * 2u;
    if (p + 1u >= lengths.a_len) {
        return;
    }
    const uint k = guard_mod(p, lengths.ir_len);
    const float ar = a[guard_index(p, lengths.a_len)];
    const float ai = a[guard_index(p + 1u, lengths.a_len)];
    const float br = ir[guard_index(k, lengths.ir_len)];
    const float bi = ir[guard_index(k + 1u, lengths.ir_len)];
    out[guard_index(p, lengths.out_len)] = ar * br - ai * bi;
    out[guard_index(p + 1u, lengths.out_len)] = ar * bi + ai * br;
}

// Planted negative only: the pre-WP-Q comparator's unmatched kernel.
kernel void complex_multiply(const device float* a [[buffer(0)]],
                             device float* out [[buffer(1)]],
                             constant MulParams& lengths [[buffer(3)]],
                             uint3 gid [[thread_position_in_grid]]) {
    const uint p = gid.x * 2u;
    if (p + 1u >= lengths.a_len) return;
    const float ar = a[p], ai = a[p + 1u];
    out[p] = ar * 1.75f - ai * 0.25f;
    out[p + 1u] = ar * 0.25f + ai * 1.75f;
}
)msl";

// ---------------------------------------------------------------------------
// Workload: a decaying random IR of `frames` taps, convolved by an N=2*frames
// FFT, with eight deterministic input patterns and double-precision oracles.

Workload make_workload(std::uint32_t frames, std::uint32_t seed) {
    Workload w;
    w.frames = frames;
    w.fft_size = frames * 2;
    w.ir_length = frames;
    for (auto n = w.fft_size; n > 1; n >>= 1)
        ++w.log2;
    w.complex_floats = static_cast<std::size_t>(kChannels) * w.fft_size * 2;
    w.period_ns = static_cast<double>(frames) * 1.0e9 / kSampleRate;

    std::mt19937 rng(seed + frames);
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    std::vector<double> ir(w.ir_length);
    for (std::uint32_t t = 0; t < w.ir_length; ++t)
        ir[t] = 0.5 * uniform(rng) * std::exp(-static_cast<double>(t) / (w.ir_length / 4.0));

    const auto n = w.fft_size;
    w.ir_spectrum.assign(static_cast<std::size_t>(n) * 2, 0.0f);
    for (std::uint32_t bin = 0; bin < n; ++bin) {
        double re = 0.0;
        double im = 0.0;
        for (std::uint32_t t = 0; t < w.ir_length; ++t) {
            const double angle = -2.0 * M_PI * static_cast<double>(bin) * t / n;
            re += ir[t] * std::cos(angle);
            im += ir[t] * std::sin(angle);
        }
        w.ir_spectrum[bin * 2] = static_cast<float>(re / n);
        w.ir_spectrum[bin * 2 + 1] = static_cast<float>(im / n);
    }

    for (std::uint32_t p = 0; p < kPatterns; ++p) {
        auto& input = w.inputs[p];
        auto& expected = w.expected[p];
        input.assign(w.complex_floats, 0.0f);
        expected.assign(static_cast<std::size_t>(kChannels) * n, 0.0f);
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            std::vector<float> x(frames);
            for (std::uint32_t f = 0; f < frames; ++f) {
                x[f] = static_cast<float>(0.8 * uniform(rng));
                input[(static_cast<std::size_t>(c) * n + f) * 2] = x[f];
            }
            for (std::uint32_t out = 0; out < n; ++out) {
                double acc = 0.0;
                for (std::uint32_t t = 0; t < w.ir_length; ++t)
                    if (out >= t && out - t < frames)
                        acc += ir[t] * static_cast<double>(x[out - t]);
                expected[static_cast<std::size_t>(c) * n + out] = static_cast<float>(acc);
            }
        }
    }
    return w;
}

// ---------------------------------------------------------------------------
// Per-block observation shared by both sides

struct BlockRecord {
    std::int64_t release_ns = -1;
    std::int64_t submit_ns = -1;
    std::int64_t submit_end_ns = -1;
    std::int64_t visible_ns = -1;
    std::int64_t gpu_busy_ns = -1;
    std::int64_t gpu_end_ns = -1;
    std::int64_t callback_ns = -1;
    std::int64_t callback_cpu_ns = -1;
    std::int64_t worker_cpu_ns = -1;
    int oracle = 0;
    int stalled = 0;
};

struct Observer {
    const Workload* workload = nullptr;
    std::vector<BlockRecord>* records = nullptr;
    std::uint32_t warmup = 0;
    std::array<std::vector<float>, kPatterns>* parity = nullptr;
    // Oracle and parity work is subtracted from the worker CPU samples.
    std::uint64_t excluded_cpu_ns = 0;
    double max_oracle_error = 0.0;

    // `output` is interleaved complex; the real parts are the audio samples.
    void visible(std::uint64_t sequence, std::uint64_t visible_ns, const float* output) {
        auto& record = (*records)[sequence];
        record.visible_ns = static_cast<std::int64_t>(visible_ns);
        const auto cpu_begin = thread_cpu_ns();
        const auto pattern = sequence % kPatterns;
        const auto& expected = workload->expected[pattern];
        double error = 0.0;
        bool ok = output != nullptr;
        for (std::size_t i = 0; ok && i < expected.size(); ++i) {
            const float value = output[i * 2];
            if (!std::isfinite(value)) {
                ok = false;
                break;
            }
            error = std::max(error, static_cast<double>(std::fabs(value - expected[i])));
        }
        ok = ok && error <= kOracleTolerance;
        record.oracle = ok ? 1 : 0;
        max_oracle_error = std::max(max_oracle_error, error);
        // The last kPatterns warm-up blocks cover every pattern once.
        if (output != nullptr && sequence + kPatterns >= warmup && sequence < warmup) {
            auto& slot = (*parity)[pattern];
            slot.resize(expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i)
                slot[i] = output[i * 2];
        }
        excluded_cpu_ns += thread_cpu_ns() - cpu_begin;
    }
};

// ---------------------------------------------------------------------------
// Native Metal side

struct AlignedBytes {
    void* pointer = nullptr;
    std::size_t length = 0;
    explicit AlignedBytes(std::size_t bytes) {
        const auto page = static_cast<std::size_t>(getpagesize());
        length = (bytes + page - 1) / page * page;
        if (posix_memalign(&pointer, page, length) == 0)
            std::memset(pointer, 0, length);
        else
            pointer = nullptr;
    }
    ~AlignedBytes() {
        std::free(pointer);
    }
    AlignedBytes(const AlignedBytes&) = delete;
    AlignedBytes& operator=(const AlignedBytes&) = delete;
};

enum class MetalCompletion { SharedEvent, CompletedHandler };

struct MetalPipelines {
    // Guarded (Tint-equivalent) kernels gate; unguarded ones only feed the
    // labelled robustness-cost probe.
    id<MTLComputePipelineState> fft = nil;
    id<MTLComputePipelineState> multiply = nil;
    id<MTLComputePipelineState> fft_unguarded = nil;
    id<MTLComputePipelineState> multiply_unguarded = nil;
    id<MTLComputePipelineState> planted = nil;
    std::string msl_sha256;
    std::string math_mode;
};

class MetalLane {
  public:
    MetalLane(id<MTLDevice> device, const MetalPipelines& pipelines, MetalCompletion completion,
              bool guarded, bool planted)
        : device_(device), pipelines_(pipelines), completion_(completion), guarded_(guarded),
          planted_(planted) {}

    // Completion blocks hold `slots_` (and with it the no-copy host memory)
    // until they run, so a failed cell can be torn down with work in flight.
    ~MetalLane() {
        finish();
        if (listener_queue_)
            dispatch_sync(listener_queue_, ^{
                          });
    }

    bool prepare(const Workload& w, std::uint32_t total, std::string& reason) {
        workload_ = &w;
        passes_ = convolution_passes(w);
        queue_ = [device_ newCommandQueue];
        semaphore_ = dispatch_semaphore_create(0);
        if (!queue_ || !semaphore_) {
            reason = "queue_unavailable";
            return false;
        }
        const auto bytes = w.complex_floats * sizeof(float);
        const auto floats = static_cast<std::uint32_t>(w.complex_floats);
        ir_ = [device_ newBufferWithBytes:w.ir_spectrum.data()
                                   length:w.ir_spectrum.size() * sizeof(float)
                                  options:MTLResourceStorageModeShared];
        const std::uint32_t mul_lengths[3] = {floats, w.fft_size * 2, floats};
        mul_params_ = [device_ newBufferWithBytes:mul_lengths
                                           length:sizeof(mul_lengths)
                                          options:MTLResourceStorageModeShared];
        const std::uint32_t fft_lengths[2] = {floats, floats};
        fft_lengths_ = [device_ newBufferWithBytes:fft_lengths
                                            length:sizeof(fft_lengths)
                                           options:MTLResourceStorageModeShared];
        struct Params {
            std::uint32_t n;
            std::uint32_t ns;
            float sign;
            std::uint32_t batch;
        };
        for (std::uint32_t stage = 0; stage < w.log2; ++stage) {
            const Params forward{w.fft_size, 1u << stage, -1.0f, kChannels};
            const Params inverse{w.fft_size, 1u << stage, 1.0f, kChannels};
            forward_.push_back([device_ newBufferWithBytes:&forward
                                                    length:sizeof(forward)
                                                   options:MTLResourceStorageModeShared]);
            inverse_.push_back([device_ newBufferWithBytes:&inverse
                                                    length:sizeof(inverse)
                                                   options:MTLResourceStorageModeShared]);
        }
        for (auto& slot : shared_->slots) {
            slot.input = std::make_unique<AlignedBytes>(bytes);
            slot.output = std::make_unique<AlignedBytes>(bytes);
            if (!slot.input->pointer || !slot.output->pointer) {
                reason = "host_allocation_failed";
                return false;
            }
            slot.input_buffer = [device_ newBufferWithBytesNoCopy:slot.input->pointer
                                                           length:slot.input->length
                                                          options:MTLResourceStorageModeShared
                                                      deallocator:nil];
            slot.output_buffer = [device_ newBufferWithBytesNoCopy:slot.output->pointer
                                                            length:slot.output->length
                                                           options:MTLResourceStorageModeShared
                                                       deallocator:nil];
            slot.a = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
            slot.b = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
            slot.product = [device_ newBufferWithLength:bytes
                                                options:MTLResourceStorageModePrivate];
            if (!slot.input_buffer || !slot.output_buffer || !slot.a || !slot.b || !slot.product) {
                reason = "buffer_unavailable";
                return false;
            }
        }
        if (!ir_ || !mul_params_ || !fft_lengths_ || forward_.size() != w.log2 ||
            inverse_.size() != w.log2) {
            reason = "buffer_unavailable";
            return false;
        }
        if (completion_ == MetalCompletion::SharedEvent) {
            event_ = [device_ newSharedEvent];
            dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
                DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
            listener_queue_ = dispatch_queue_create("pulp.comparator.metal.listener", attr);
            listener_ = [[MTLSharedEventListener alloc] initWithDispatchQueue:listener_queue_];
            if (!event_ || !listener_) {
                reason = "shared_event_unavailable";
                return false;
            }
        }
        gpu_start_.assign(total, -1);
        gpu_end_.assign(total, -1);
        shared_->callback_cpu = std::make_unique<std::atomic<std::int64_t>[]>(total);
        for (std::uint32_t i = 0; i < total; ++i)
            shared_->callback_cpu[i].store(-1, std::memory_order_relaxed);
        shared_->total = total;
        return true;
    }

    bool can_submit(std::uint64_t sequence) const {
        return shared_->slots[sequence % kFlightDepth].state.load(std::memory_order_acquire) == 0;
    }

    std::size_t in_flight() const {
        return in_flight_;
    }

    bool submit(std::uint64_t sequence, const float* input) {
        @autoreleasepool {
            auto& slot = shared_->slots[sequence % kFlightDepth];
            harvest_gpu_times(history_[sequence % kHistory]);
            std::memcpy(slot.input->pointer, input, workload_->complex_floats * sizeof(float));
            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (!command)
                return false;
            const auto& w = *workload_;
            const MTLSize threads = MTLSizeMake(kWorkgroup, 1, 1);
            const auto pass = [&](id<MTLComputePipelineState> pipeline, id<MTLBuffer> first,
                                  id<MTLBuffer> second, id<MTLBuffer> third, id<MTLBuffer> fourth,
                                  std::uint32_t groups) {
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:first offset:0 atIndex:0];
                [encoder setBuffer:second offset:0 atIndex:1];
                if (third)
                    [encoder setBuffer:third offset:0 atIndex:2];
                if (fourth)
                    [encoder setBuffer:fourth offset:0 atIndex:3];
                [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                        threadsPerThreadgroup:threads];
                [encoder endEncoding];
            };
            if (planted_) {
                pass(pipelines_.planted, slot.input_buffer, slot.output_buffer, nil, mul_params_,
                     planted_groups(w));
            } else {
                const auto fft = guarded_ ? pipelines_.fft : pipelines_.fft_unguarded;
                const auto multiply =
                    guarded_ ? pipelines_.multiply : pipelines_.multiply_unguarded;
                // Same ping-pong as the provider's convolution plan.
                id<MTLBuffer> source = slot.input_buffer;
                id<MTLBuffer> destination = slot.a;
                for (std::uint32_t stage = 0; stage < w.log2; ++stage) {
                    pass(fft, source, destination, forward_[stage], fft_lengths_,
                         passes_[stage].groups);
                    source = destination;
                    destination = destination == slot.a ? slot.b : slot.a;
                }
                pass(multiply, source, ir_, slot.product, mul_params_, passes_[w.log2].groups);
                source = slot.product;
                destination = slot.a;
                for (std::uint32_t stage = 0; stage < w.log2; ++stage) {
                    if (stage + 1u == w.log2)
                        destination = slot.output_buffer;
                    pass(fft, source, destination, inverse_[stage], fft_lengths_,
                         passes_[w.log2 + 1 + stage].groups);
                    source = destination;
                    if (stage + 1u < w.log2)
                        destination = destination == slot.a ? slot.b : slot.a;
                }
            }
            slot.sequence = sequence;
            slot.callback_ns.store(-1, std::memory_order_relaxed);
            slot.state.store(1, std::memory_order_release);
            // Blocks capture the shared state, never `this`. The completion
            // block's own thread CPU is recorded per block, since Dawn's
            // equivalent work runs on the measured worker.
            auto shared = shared_;
            const auto index = sequence % kFlightDepth;
            dispatch_semaphore_t semaphore = semaphore_;
            const auto complete = ^{
              const auto cpu_begin = thread_cpu_ns();
              auto& target = shared->slots[index];
              target.callback_ns.store(static_cast<std::int64_t>(now_ns()),
                                       std::memory_order_relaxed);
              target.state.store(2, std::memory_order_release);
              dispatch_semaphore_signal(semaphore);
              if (sequence < shared->total)
                  shared->callback_cpu[sequence].store(
                      static_cast<std::int64_t>(thread_cpu_ns() - cpu_begin),
                      std::memory_order_relaxed);
            };
            if (completion_ == MetalCompletion::SharedEvent) {
                const auto value = sequence + 1;
                [command encodeSignalEvent:event_ value:value];
                [event_ notifyListener:listener_
                               atValue:value
                                 block:^(id<MTLSharedEvent>, uint64_t) {
                                   complete();
                                 }];
                // Keeps the slots (and the no-copy memory) alive until the
                // GPU is done with this command buffer.
                [command addCompletedHandler:^(id<MTLCommandBuffer>) {
                  (void)shared;
                }];
            } else {
                [command addCompletedHandler:^(id<MTLCommandBuffer>) {
                  complete();
                }];
            }
            history_[sequence % kHistory] = {command, sequence};
            [command commit];
            fifo_[(fifo_head_ + in_flight_) % kFlightDepth] = sequence;
            ++in_flight_;
            return true;
        }
    }

    void wait(std::uint64_t deadline) {
        const auto now = now_ns();
        if (in_flight_ == 0 || deadline <= now) {
            sleep_until_ns(deadline);
            return;
        }
        dispatch_semaphore_wait(
            semaphore_,
            dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(deadline - now)));
    }

    void collect(Observer& observer, std::vector<BlockRecord>& records) {
        collect_completed(observer, records);
        // Drop surplus signals so the next wait sleeps until a real
        // completion, then re-check: a completion whose signal was just
        // drained has already published state 2.
        while (dispatch_semaphore_wait(semaphore_, DISPATCH_TIME_NOW) == 0) {
        }
        collect_completed(observer, records);
    }

    void finish() {
        for (auto& entry : history_)
            harvest_gpu_times(entry);
    }

    // Completion-block CPU per sequence; -1 where the block had not finished
    // when the cell ended.
    std::int64_t callback_cpu(std::uint64_t sequence) const {
        return sequence < shared_->total
                   ? shared_->callback_cpu[sequence].load(std::memory_order_relaxed)
                   : -1;
    }

    const std::vector<std::int64_t>& gpu_start() const {
        return gpu_start_;
    }
    const std::vector<std::int64_t>& gpu_end() const {
        return gpu_end_;
    }
    std::uint64_t late_gpu_time_waits() const {
        return late_gpu_time_waits_;
    }
    std::uint64_t command_errors() const {
        return command_errors_;
    }

    static std::uint32_t planted_groups(const Workload& w) {
        return static_cast<std::uint32_t>((w.complex_floats / 2 + kWorkgroup - 1) / kWorkgroup);
    }

  private:
    struct Slot {
        std::unique_ptr<AlignedBytes> input;
        std::unique_ptr<AlignedBytes> output;
        id<MTLBuffer> input_buffer = nil;
        id<MTLBuffer> output_buffer = nil;
        id<MTLBuffer> a = nil;
        id<MTLBuffer> b = nil;
        id<MTLBuffer> product = nil;
        std::uint64_t sequence = 0;
        std::atomic<int> state{0}; // 0 free, 1 in flight, 2 completed
        std::atomic<std::int64_t> callback_ns{-1};
    };
    struct Shared {
        std::array<Slot, kFlightDepth> slots;
        std::unique_ptr<std::atomic<std::int64_t>[]> callback_cpu;
        std::uint32_t total = 0;
    };
    struct Submitted {
        id<MTLCommandBuffer> command = nil;
        std::uint64_t sequence = 0;
    };
    // GPU times are read kHistory blocks after submission, when the command
    // buffer has long completed, so harvesting never blocks a submit. A shared
    // event can fire before its command buffer reports Completed.
    static constexpr std::size_t kHistory = 64;

    void collect_completed(Observer& observer, std::vector<BlockRecord>& records) {
        while (in_flight_ != 0) {
            const auto sequence = fifo_[fifo_head_];
            auto& slot = shared_->slots[sequence % kFlightDepth];
            if (slot.state.load(std::memory_order_acquire) != 2)
                break;
            const auto visible = now_ns();
            records[sequence].callback_ns = slot.callback_ns.load(std::memory_order_relaxed);
            observer.visible(sequence, visible, static_cast<const float*>(slot.output->pointer));
            fifo_head_ = (fifo_head_ + 1) % kFlightDepth;
            --in_flight_;
            slot.state.store(0, std::memory_order_release);
        }
    }

    void harvest_gpu_times(Submitted& entry) {
        if (!entry.command)
            return;
        if (entry.command.status < MTLCommandBufferStatusCompleted) {
            ++late_gpu_time_waits_;
            [entry.command waitUntilCompleted];
        }
        if (entry.command.status != MTLCommandBufferStatusCompleted)
            ++command_errors_;
        else if (entry.sequence < gpu_start_.size() && entry.command.GPUEndTime > 0.0) {
            gpu_start_[entry.sequence] = clock_.from_metal_seconds(entry.command.GPUStartTime);
            gpu_end_[entry.sequence] = clock_.from_metal_seconds(entry.command.GPUEndTime);
        }
        entry.command = nil;
    }

    id<MTLDevice> device_;
    const MetalPipelines& pipelines_;
    MetalCompletion completion_;
    bool guarded_;
    bool planted_;
    const Workload* workload_ = nullptr;
    std::vector<PassShape> passes_;
    id<MTLCommandQueue> queue_ = nil;
    dispatch_semaphore_t semaphore_ = nullptr;
    id<MTLSharedEvent> event_ = nil;
    dispatch_queue_t listener_queue_ = nil;
    MTLSharedEventListener* listener_ = nil;
    id<MTLBuffer> ir_ = nil;
    id<MTLBuffer> mul_params_ = nil;
    id<MTLBuffer> fft_lengths_ = nil;
    std::vector<id<MTLBuffer>> forward_;
    std::vector<id<MTLBuffer>> inverse_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::array<std::uint64_t, kFlightDepth> fifo_{};
    std::size_t fifo_head_ = 0;
    std::size_t in_flight_ = 0;
    std::array<Submitted, kHistory> history_{};
    std::vector<std::int64_t> gpu_start_;
    std::vector<std::int64_t> gpu_end_;
    std::uint64_t late_gpu_time_waits_ = 0;
    std::uint64_t command_errors_ = 0;
    HostClockBridge clock_;
};

bool make_metal_pipelines(id<MTLDevice> device, MetalPipelines& out, std::string& reason) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
        out.math_mode = "safe";
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        out.math_mode = "fast_math_disabled";
    }
    NSError* error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:[NSString stringWithUTF8String:kMatchedMsl]
                             options:options
                               error:&error];
    if (!library) {
        reason = "msl_compile_failed";
        return false;
    }
    const auto make = [&](NSString* name, bool guarded) -> id<MTLComputePipelineState> {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&guarded type:MTLDataTypeBool atIndex:0];
        id<MTLFunction> function = [library newFunctionWithName:name
                                                 constantValues:constants
                                                          error:&error];
        return function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    };
    out.fft = make(@"stockham_fft", true);
    out.multiply = make(@"complex_multiply_ir", true);
    out.fft_unguarded = make(@"stockham_fft", false);
    out.multiply_unguarded = make(@"complex_multiply_ir", false);
    out.planted = make(@"complex_multiply", true);
    out.msl_sha256 = sha256_hex(std::string(kMatchedMsl));
    if (!out.fft || !out.multiply || !out.fft_unguarded || !out.multiply_unguarded ||
        !out.planted || out.fft.maxTotalThreadsPerThreadgroup < kWorkgroup ||
        out.multiply.maxTotalThreadsPerThreadgroup < kWorkgroup) {
        reason = "pipeline_unavailable";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dawn side (production DawnSharedIoProvider + SharedIoComputePlan)

#if PULP_HAS_DAWN_COMPARATOR
using pulp::gpu_audio::detail::DawnSharedIoProvider;
using pulp::gpu_audio::detail::SharedIoArena;
using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoComputePlan;

struct DawnIdentity {
    bool observed = false;
    std::string provider_revision;
    bool authenticated = false;
    bool native_runtime_authenticated = false;
    std::string adapter_name;
    std::string adapter_backend;
    std::string native_runtime_name;
    std::string native_runtime_revision;
    std::string dawn_revision;
};

class DawnLane {
  public:
    DawnLane(DawnSharedIoProvider::CompletionPolicy policy, bool staged_timestamps,
             std::uint64_t poll_interval_ns)
        : policy_(policy), staged_timestamps_(staged_timestamps),
          poll_interval_ns_(poll_interval_ns) {}

    ~DawnLane() {
        if (plan_.prepared())
            (void)plan_.release();
    }

    bool prepare(const Workload& w, std::uint32_t total, std::string& reason,
                 DawnIdentity& identity) {
        workload_ = &w;
        DawnSharedIoProvider::Options options;
        options.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA;
        options.completion_policy = policy_;
        if (staged_timestamps_) {
            options.storage_kind = SharedIoArenaProvider::StorageKind::Staged;
            options.enable_timestamps = true;
        }
        auto created = DawnSharedIoProvider::create(options);
        if (!created.provider) {
            reason = created.reason.empty() ? "provider_unavailable" : created.reason;
            return false;
        }
        provider_ = std::move(created.provider);
        auto program =
            provider_->make_convolution_program({.fft_size = w.fft_size,
                                                 .channels = kChannels,
                                                 .logical_frames = w.frames,
                                                 .ir_length = w.ir_length,
                                                 .normalized_ir_spectrum = w.ir_spectrum});
        const auto bytes = w.complex_floats * sizeof(float);
        SharedIoComputePlan::Config config{
            .slots = kFlightDepth, .input_bytes_per_slot = bytes, .output_bytes_per_slot = bytes};
        if (staged_timestamps_)
            config.storage_kind = SharedIoArenaProvider::StorageKind::Staged;
        if (!program || !plan_.prepare(*provider_, config, std::move(program))) {
            reason = "prepare_failed";
            return false;
        }
        if (!identity.observed) {
            const auto id = provider_->provider_identity();
            const auto adapter = provider_->adapter_identity();
            identity.observed = true;
            identity.provider_revision = id.provider_revision;
            identity.authenticated = id.authenticated;
            identity.native_runtime_authenticated = id.native_runtime_authenticated;
            identity.adapter_name = adapter.name;
            identity.adapter_backend = adapter.backend;
            identity.native_runtime_name = adapter.native_runtime_name;
            identity.native_runtime_revision = adapter.native_runtime_revision;
            identity.dawn_revision = provider_->dawn_revision();
        }
        timestamps_enabled_ = provider_->timestamps_enabled();
        gpu_busy_.assign(total, -1);
        return true;
    }

    bool can_submit(std::uint64_t) const {
        return in_flight_ < kFlightDepth && plan_.available_slots() != 0;
    }

    std::size_t in_flight() const {
        return in_flight_;
    }

    bool submit(std::uint64_t sequence, const float* input) {
        auto write = plan_.acquire_input(sequence + 1, 0);
        if (!write)
            return false;
        std::memcpy(write->bytes.data(), input, workload_->complex_floats * sizeof(float));
        if (!plan_.submit({write->token, 0}))
            return false;
        ++in_flight_;
        return true;
    }

    void wait(std::uint64_t deadline) {
        const auto now = now_ns();
        if (in_flight_ == 0 || deadline <= now) {
            sleep_until_ns(deadline);
            return;
        }
        if (policy_ == DawnSharedIoProvider::CompletionPolicy::TimedWaitAny) {
            // Event-driven: block in WaitAny on the queue futures. When none is
            // pending (queue work done, scope or map callbacks still due),
            // WaitAny cannot block, so fall back to the transport's poll
            // quantum instead of spinning; the fallbacks are counted.
            const auto waits_before = provider_->stats().wait_any_timed_calls;
            provider_->service_until(deadline);
            if (provider_->stats().wait_any_timed_calls == waits_before &&
                plan_.drain(now_ns()) == 0) {
                ++fallback_sleeps_;
                const auto after = now_ns();
                if (after < deadline)
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(std::min(kDawnPollIntervalNs, deadline - after)));
            }
            return;
        }
        // Polling policies mirror the transport worker: poll, then sleep one
        // poll quantum when nothing retired.
        provider_->service_until(0);
        if (plan_.drain(now_ns()) == 0)
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(std::min(poll_interval_ns_, deadline - now)));
    }

    void collect(Observer& observer, std::vector<BlockRecord>&) {
        if (in_flight_ == 0)
            return;
        if (policy_ != DawnSharedIoProvider::CompletionPolicy::TimedWaitAny)
            provider_->service_until(0);
        plan_.drain(now_ns());
        while (auto completion = plan_.pop_completion()) {
            const auto sequence = completion->token.slot.stream_sequence - 1;
            auto output = plan_.acquire_output(*completion);
            const auto visible = now_ns();
            const bool ok = output.has_value() &&
                            completion->status == SharedIoArena::CompletionStatus::RetiredSuccess;
            if (completion->gpu_elapsed_available && sequence < gpu_busy_.size())
                gpu_busy_[sequence] = static_cast<std::int64_t>(completion->gpu_elapsed_ns);
            observer.visible(sequence, visible,
                             ok ? reinterpret_cast<const float*>(output->bytes.data()) : nullptr);
            if (output) {
                const auto token = output->token;
                output.reset();
                (void)plan_.release_output({token});
            }
            if (in_flight_ != 0)
                --in_flight_;
        }
    }

    void finish() {}

    bool release() {
        const bool released = plan_.release();
        provider_.reset();
        return released;
    }

    const std::vector<std::int64_t>& gpu_busy() const {
        return gpu_busy_;
    }
    bool timestamps_enabled() const {
        return timestamps_enabled_;
    }
    DawnSharedIoProvider::Stats stats() const {
        return provider_ ? provider_->stats() : DawnSharedIoProvider::Stats{};
    }
    std::uint64_t fallback_sleeps() const {
        return fallback_sleeps_;
    }

  private:
    DawnSharedIoProvider::CompletionPolicy policy_;
    bool staged_timestamps_;
    std::uint64_t poll_interval_ns_;
    const Workload* workload_ = nullptr;
    std::unique_ptr<DawnSharedIoProvider> provider_;
    SharedIoComputePlan plan_;
    std::size_t in_flight_ = 0;
    bool timestamps_enabled_ = false;
    std::uint64_t fallback_sleeps_ = 0;
    std::vector<std::int64_t> gpu_busy_;
};
#endif

// ---------------------------------------------------------------------------
// Concurrent Skia render load (ADR Measurement C) on Pulp's own UI device,
// paced at 60 fps, with a full readback per frame.

struct RenderStats {
    bool active = false;
    std::string status = "not_requested";
    std::vector<std::int64_t> frame_ns;
    std::uint64_t dropped = 0;
    std::uint64_t thread_cpu_ns = 0;
};

class RenderLoad {
  public:
    static constexpr std::uint32_t kWidth = 1280;
    static constexpr std::uint32_t kHeight = 800;
    static constexpr double kFramePeriodNs = 1.0e9 / 60.0;

    ~RenderLoad() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    bool ensure(std::string& reason) {
#if defined(PULP_COMPARATOR_HAS_RENDER_LOAD)
        if (thread_.joinable())
            return ready_;
        // The thread owns the promise, so set_value never touches a promise
        // this frame has already destroyed.
        auto started = std::make_shared<std::promise<bool>>();
        auto future = started->get_future();
        thread_ = std::thread([this, started] { run(*started); });
        ready_ = future.get();
        if (!ready_)
            reason = error_.empty() ? "render_surface_unavailable" : error_;
        return ready_;
#else
        reason = "render_load_not_built";
        return false;
#endif
    }

    void start() {
        std::lock_guard lock(mutex_);
        stats_ = {};
        stats_.active = true;
        stats_.status = "running";
        active_ = true;
        cv_.notify_all();
    }

    // Cumulative render-thread CPU, sampled around each cell so it can be
    // removed from process CPU.
    std::uint64_t thread_cpu_total() const {
        return cpu_total_.load(std::memory_order_relaxed);
    }

    RenderStats stop() {
        std::unique_lock lock(mutex_);
        active_ = false;
        // A frame takes milliseconds; a render thread that never goes idle
        // is reported, not waited on forever.
        const bool idle =
            idle_cv_.wait_for(lock, std::chrono::seconds(30), [&] { return !rendering_; });
        stats_.status = idle ? "completed" : "stop_timeout";
        return stats_;
    }

  private:
#if defined(PULP_COMPARATOR_HAS_RENDER_LOAD)
    static void paint(pulp::canvas::Canvas& canvas, std::uint64_t frame) {
        using pulp::canvas::Color;
        const float phase = static_cast<float>(frame % 240) / 240.0f;
        canvas.set_fill_color(Color::rgba(0.06f, 0.07f, 0.09f));
        canvas.fill_rect(0, 0, kWidth, kHeight);
        const Color stops[3] = {Color::rgba(0.9f, 0.3f, 0.2f, 0.8f),
                                Color::rgba(0.2f, 0.6f, 0.9f, 0.8f),
                                Color::rgba(0.3f, 0.9f, 0.4f, 0.8f)};
        const float positions[3] = {0.0f, 0.5f, 1.0f};
        for (int i = 0; i < 320; ++i) {
            const auto x = static_cast<float>((i * 37) % static_cast<int>(kWidth));
            const auto y = static_cast<float>((i * 53 + static_cast<int>(phase * 400)) %
                                              static_cast<int>(kHeight));
            canvas.save();
            canvas.translate(x, y);
            canvas.rotate(phase * 6.2831853f + static_cast<float>(i) * 0.1f);
            canvas.set_fill_gradient_linear(0, 0, 80, 40, stops, positions, 3);
            canvas.fill_rounded_rect(-40, -20, 80, 40, 8);
            canvas.clear_fill_gradient();
            canvas.restore();
        }
        canvas.set_line_width(2.0f);
        for (int i = 0; i < 160; ++i) {
            canvas.set_stroke_color(Color::rgba(0.8f, 0.8f, 0.2f + 0.004f * i, 0.6f));
            canvas.begin_path();
            const float y = 5.0f * static_cast<float>(i);
            canvas.move_to(0, y);
            canvas.cubic_to(400, y + 120 * std::sin(phase * 6.28f + i), 880,
                            y - 120 * std::cos(phase * 6.28f + i), kWidth, y);
            canvas.stroke_current_path();
        }
        for (int i = 0; i < 200; ++i) {
            canvas.set_fill_color(Color::rgba(0.1f + 0.004f * i, 0.4f, 0.8f, 0.35f));
            canvas.fill_circle(static_cast<float>((i * 97) % static_cast<int>(kWidth)),
                               static_cast<float>((i * 61) % static_cast<int>(kHeight)),
                               6.0f + static_cast<float>(i % 24));
        }
    }

    void run(std::promise<bool>& started) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pulp::render::HeadlessSurface::Config config;
        config.width = kWidth;
        config.height = kHeight;
        auto surface = pulp::render::HeadlessSurface::create(config, &error_);
        const bool ok = surface && surface->is_ready() &&
                        !surface->render_rgba([](pulp::canvas::Canvas& c) { paint(c, 0); }).empty();
        started.set_value(ok);
        if (!ok)
            return;
        std::uint64_t frame = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                while (!cv_.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return quit_ || active_; })) {
                }
                if (quit_)
                    return;
                rendering_ = true;
            }
            const auto cpu_begin = thread_cpu_ns();
            const auto t0 = now_ns();
            std::uint64_t index = 0;
            std::vector<std::int64_t> frames;
            std::uint64_t dropped = 0;
            for (;;) {
                {
                    std::lock_guard lock(mutex_);
                    if (!active_ || quit_)
                        break;
                }
                sleep_until_ns(t0 + static_cast<std::uint64_t>(index * kFramePeriodNs));
                const auto begin = now_ns();
                @autoreleasepool {
                    (void)surface->render_rgba(
                        [frame](pulp::canvas::Canvas& c) { paint(c, frame); });
                }
                const auto end = now_ns();
                cpu_total_.store(thread_cpu_ns(), std::memory_order_relaxed);
                frames.push_back(static_cast<std::int64_t>(end - begin));
                ++frame;
                // Frame slots whose deadline already passed are dropped.
                const auto next = static_cast<std::uint64_t>((end - t0) / kFramePeriodNs) + 1;
                if (next > index + 1)
                    dropped += next - index - 1;
                index = std::max(next, index + 1);
            }
            std::lock_guard lock(mutex_);
            stats_.frame_ns = std::move(frames);
            stats_.dropped = dropped;
            stats_.thread_cpu_ns = thread_cpu_ns() - cpu_begin;
            rendering_ = false;
            idle_cv_.notify_all();
        }
    }
#endif

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::thread thread_;
    bool ready_ = false;
    bool active_ = false;
    bool rendering_ = false;
    bool quit_ = false;
    std::string error_;
    RenderStats stats_;
    std::atomic<std::uint64_t> cpu_total_{0};
};

// ---------------------------------------------------------------------------
// Paced cell driver

struct LaneSpec {
    std::string name;
    std::string side; // "metal" | "dawn"
    MetalCompletion metal_completion = MetalCompletion::SharedEvent;
    int dawn_policy = 0; // 0 ProcessEvents, 1 WaitAny, 2 TimedWaitAny
    bool dawn_staged_timestamps = false;
    std::uint64_t poll_interval_ns = 0;
    bool production_eligible = true;
    // Metal only: Tint-equivalent guards. Unguarded lanes are the labelled
    // robustness-cost probe and never gate.
    bool guarded = true;
    std::string role = "gating";
};

struct CellResult {
    std::string lane;
    std::string side;
    std::uint32_t frames = 0;
    std::string load;
    std::string status = "completed";
    std::string reason;
    KernelIdentity kernel;
    std::vector<BlockRecord> records;
    std::uint64_t oracle_failures = 0;
    double max_oracle_error = 0.0;
    std::uint64_t stalls = 0;
    std::uint64_t worker_cpu_total_ns = 0;
    std::uint64_t process_cpu_total_ns = 0;
    std::uint64_t render_cpu_in_window_ns = 0;
    std::uint64_t oracle_cpu_ns = 0;
    std::uint64_t driven_blocks = 0;
    std::string role = "gating";
    bool gpu_timestamps = false;
    std::string gpu_busy_domain;
    std::string wake_domain;
    std::uint64_t late_gpu_time_waits = 0;
    std::string extra_json;
    RenderStats render;
    std::array<std::vector<float>, kPatterns> parity;
    std::uint64_t t0_ns = 0;
};

struct RunConfig {
    std::uint32_t blocks = 10'000;
    std::uint32_t warmup = 512;
    std::vector<std::uint32_t> frames = {64, 128, 256};
    std::vector<std::string> loads = {"idle", "render"};
    std::vector<std::string> lanes;
    std::uint32_t seed = 20261022;
    bool planted = false;
    std::string output;
    std::string mode = "campaign";
};

// Block k is released at t0 + k * period. Between releases the worker only
// services completions; worker CPU for interval k is the thread CPU from
// release k to release k+1, minus the oracle work.
//
// Process CPU over the same window, minus the render thread and the oracle,
// is the CPU measure the gate uses: it counts completion work wherever it
// runs (Dawn on the worker, Metal on its own dispatch threads).
template <class Lane>
void drive(Lane& lane, const Workload& w, const RunConfig& rc, CellResult& cell,
           const std::function<std::uint64_t()>& render_cpu) {
    const std::uint32_t total = rc.warmup + rc.blocks;
    std::vector<BlockRecord> records(total);
    Observer observer;
    observer.workload = &w;
    observer.records = &records;
    observer.warmup = rc.warmup;
    observer.parity = &cell.parity;

    const auto period = w.period_ns;
    const auto t0 = now_ns() + 5'000'000;
    cell.t0_ns = t0;
    sleep_until_ns(t0 - 1'000'000);
    auto cpu_mark = thread_cpu_ns();
    std::uint64_t excluded_mark = 0;
    const auto process_begin = process_cpu_ns();
    const auto render_begin = render_cpu();
    const auto worker_begin = cpu_mark;
    for (std::uint32_t k = 0; k < total; ++k) {
        const auto release = t0 + static_cast<std::uint64_t>(k * period);
        for (;;) {
            lane.collect(observer, records);
            if (now_ns() >= release)
                break;
            lane.wait(release);
        }
        const auto cpu_now = thread_cpu_ns();
        if (k > 0) {
            const auto excluded = observer.excluded_cpu_ns - excluded_mark;
            const auto used = cpu_now - cpu_mark;
            records[k - 1].worker_cpu_ns =
                static_cast<std::int64_t>(used > excluded ? used - excluded : 0);
        }
        cpu_mark = cpu_now;
        excluded_mark = observer.excluded_cpu_ns;

        bool stalled = false;
        const auto stall_begin = now_ns();
        while (!lane.can_submit(k)) {
            stalled = true;
            lane.wait(now_ns() + 1'000'000);
            lane.collect(observer, records);
            if (now_ns() - stall_begin > kStallLimitNs) {
                cell.status = "failed";
                cell.reason = "completion_stalled";
                return;
            }
        }
        auto& record = records[k];
        record.release_ns = static_cast<std::int64_t>(release - t0);
        record.stalled = stalled ? 1 : 0;
        cell.stalls += stalled ? 1 : 0;
        const auto submit_begin = now_ns();
        if (!lane.submit(k, w.inputs[k % kPatterns].data())) {
            cell.status = "failed";
            cell.reason = "submit_failed";
            return;
        }
        record.submit_ns = static_cast<std::int64_t>(submit_begin - t0);
        record.submit_end_ns = static_cast<std::int64_t>(now_ns() - t0);
    }
    const auto drain_begin = now_ns();
    while (lane.in_flight() != 0) {
        lane.wait(now_ns() + 1'000'000);
        lane.collect(observer, records);
        if (now_ns() - drain_begin > kStallLimitNs) {
            cell.status = "failed";
            cell.reason = "drain_stalled";
            return;
        }
    }
    const auto cpu_end = thread_cpu_ns();
    const auto excluded = observer.excluded_cpu_ns - excluded_mark;
    const auto tail = cpu_end - cpu_mark;
    records[total - 1].worker_cpu_ns =
        static_cast<std::int64_t>(tail > excluded ? tail - excluded : 0);
    cell.process_cpu_total_ns = process_cpu_ns() - process_begin;
    cell.render_cpu_in_window_ns = render_cpu() - render_begin;
    cell.oracle_cpu_ns = observer.excluded_cpu_ns;
    cell.driven_blocks = total;
    lane.finish();
    cell.worker_cpu_total_ns = cpu_end - worker_begin - observer.excluded_cpu_ns;
    for (std::uint32_t k = rc.warmup; k < total; ++k)
        if (!records[k].oracle)
            ++cell.oracle_failures;
    cell.max_oracle_error = observer.max_oracle_error;
    for (auto& record : records)
        if (record.visible_ns >= 0)
            record.visible_ns -= static_cast<std::int64_t>(t0);
    cell.records.assign(records.begin() + rc.warmup, records.end());
}

// Every lane runs on a dedicated worker thread at the same QoS.
template <class Fn> void on_worker_thread(Fn&& fn) {
    std::thread worker([&] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        @autoreleasepool {
            fn();
        }
    });
    worker.join();
}

std::string cell_json(const CellResult& cell, const RunConfig& rc) {
    std::vector<std::int64_t> release, submit, submit_end, visible, busy, end, callback,
        callback_cpu, cpu;
    std::vector<int> oracle, stalled;
    for (const auto& r : cell.records) {
        release.push_back(r.release_ns);
        submit.push_back(r.submit_ns);
        submit_end.push_back(r.submit_end_ns);
        visible.push_back(r.visible_ns);
        busy.push_back(r.gpu_busy_ns);
        end.push_back(r.gpu_end_ns);
        callback.push_back(r.callback_ns);
        callback_cpu.push_back(r.callback_cpu_ns);
        cpu.push_back(r.worker_cpu_ns);
        oracle.push_back(r.oracle);
        stalled.push_back(r.stalled);
    }
    std::string out =
        "{\"lane\":" + json_string(cell.lane) + ",\"side\":" + json_string(cell.side) +
        ",\"role\":" + json_string(cell.role) + ",\"frames\":" + std::to_string(cell.frames) +
        ",\"load\":" + json_string(cell.load) + ",\"status\":" + json_string(cell.status) +
        ",\"reason\":" + json_string(cell.reason) +
        ",\"blocks\":" + std::to_string(cell.records.size()) +
        ",\"warmup_blocks\":" + std::to_string(rc.warmup) + ",\"kernel\":{\"canonical\":" +
        (cell.kernel.canonical.empty() ? "null" : cell.kernel.canonical) +
        ",\"kernel_digest\":" + json_string(cell.kernel.digest) + "}" +
        ",\"oracle_failures\":" + std::to_string(cell.oracle_failures) +
        ",\"max_oracle_error\":" + json_double(cell.max_oracle_error) +
        ",\"stalls\":" + std::to_string(cell.stalls) +
        ",\"worker_cpu_total_ns\":" + std::to_string(cell.worker_cpu_total_ns) +
        ",\"process_cpu_total_ns\":" + std::to_string(cell.process_cpu_total_ns) +
        ",\"render_cpu_in_window_ns\":" + std::to_string(cell.render_cpu_in_window_ns) +
        ",\"oracle_cpu_ns\":" + std::to_string(cell.oracle_cpu_ns) +
        ",\"driven_blocks\":" + std::to_string(cell.driven_blocks) +
        ",\"gpu_timestamps\":" + json_bool(cell.gpu_timestamps) +
        ",\"gpu_busy_domain\":" + json_string(cell.gpu_busy_domain) +
        ",\"wake_domain\":" + json_string(cell.wake_domain) +
        ",\"late_gpu_time_waits\":" + std::to_string(cell.late_gpu_time_waits);
    if (!cell.extra_json.empty())
        out += "," + cell.extra_json;
    out += ",\"render_load\":{\"active\":" + std::string(json_bool(cell.render.active)) +
           ",\"status\":" + json_string(cell.render.status) +
           ",\"dropped_frames\":" + std::to_string(cell.render.dropped) +
           ",\"thread_cpu_ns\":" + std::to_string(cell.render.thread_cpu_ns) +
           ",\"frame_ns\":" + json_array(cell.render.frame_ns) + "}";
    out += ",\"samples\":{\"release_ns\":" + json_array(release) +
           ",\"submit_ns\":" + json_array(submit) + ",\"submit_end_ns\":" + json_array(submit_end) +
           ",\"visible_ns\":" + json_array(visible) + ",\"gpu_busy_ns\":" + json_array(busy) +
           ",\"gpu_end_ns\":" + json_array(end) + ",\"callback_ns\":" + json_array(callback) +
           ",\"callback_cpu_ns\":" + json_array(callback_cpu) +
           ",\"worker_cpu_ns\":" + json_array(cpu) + ",\"oracle\":" + json_array(oracle) +
           ",\"stalled\":" + json_array(stalled) + "}}";
    return out;
}

std::vector<LaneSpec> default_lanes() {
    std::vector<LaneSpec> lanes;
    lanes.push_back({.name = "metal_shared_event",
                     .side = "metal",
                     .metal_completion = MetalCompletion::SharedEvent});
    lanes.push_back({.name = "metal_completed_handler",
                     .side = "metal",
                     .metal_completion = MetalCompletion::CompletedHandler});
    // Robustness-cost probe: the same program without the Tint-equivalent
    // guards. Labelled, never production-eligible, never gating.
    lanes.push_back({.name = "metal_shared_event_unguarded_robustness_probe",
                     .side = "metal",
                     .metal_completion = MetalCompletion::SharedEvent,
                     .production_eligible = false,
                     .guarded = false,
                     .role = "robustness_cost_probe"});
    lanes.push_back({.name = "dawn_imported_timed_wait_any", .side = "dawn", .dawn_policy = 2});
    lanes.push_back({.name = "dawn_imported_process_events_poll50us",
                     .side = "dawn",
                     .dawn_policy = 0,
                     .poll_interval_ns = kDawnPollIntervalNs});
    lanes.push_back({.name = "dawn_imported_wait_any_poll50us",
                     .side = "dawn",
                     .dawn_policy = 1,
                     .poll_interval_ns = kDawnPollIntervalNs});
    // The provider refuses timestamps on imported storage, so Dawn GPU busy
    // time comes from a diagnostic staged lane that production never uses.
    lanes.push_back({.name = "dawn_staged_timestamps_timed_wait_any",
                     .side = "dawn",
                     .dawn_policy = 2,
                     .dawn_staged_timestamps = true,
                     .production_eligible = false});
    return lanes;
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
        if (!item.empty())
            out.push_back(item);
    return out;
}

bool parse_args(int argc, char** argv, RunConfig& rc) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (arg == "--blocks")
                rc.blocks = static_cast<std::uint32_t>(std::stoul(next()));
            else if (arg == "--warmup")
                rc.warmup = static_cast<std::uint32_t>(std::stoul(next()));
            else if (arg == "--frames") {
                rc.frames.clear();
                for (const auto& f : split(next()))
                    rc.frames.push_back(static_cast<std::uint32_t>(std::stoul(f)));
            } else if (arg == "--loads")
                rc.loads = split(next());
            else if (arg == "--lanes")
                rc.lanes = split(next());
            else if (arg == "--seed")
                rc.seed = static_cast<std::uint32_t>(std::stoul(next()));
            else if (arg == "--output")
                rc.output = next();
            else if (arg == "--plant-unmatched-kernel")
                rc.planted = true;
            else if (arg == "--smoke") {
                rc.blocks = 192;
                rc.warmup = 32;
                rc.mode = "smoke";
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return false;
            }
        }
    } catch (const std::exception&) {
        return false;
    }
    for (auto f : rc.frames)
        if (f < 8 || f > 4096 || (f & (f - 1)) != 0)
            return false;
    for (const auto& load : rc.loads)
        if (load != "idle" && load != "render")
            return false;
    return rc.blocks > 0 && rc.warmup >= kPatterns && !rc.frames.empty() && !rc.loads.empty();
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        RunConfig rc;
        if (!parse_args(argc, argv, rc))
            return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "comparator: no Metal device\n");
            return 77;
        }
        std::string reason;
        MetalPipelines pipelines;
        if (!make_metal_pipelines(device, pipelines, reason)) {
            std::fprintf(stderr, "comparator: %s\n", reason.c_str());
            return 1;
        }
        const auto wgsl = extract_provider_wgsl(PULP_GPU_AUDIO_PROVIDER_SOURCE);

        auto lanes = default_lanes();
        if (!rc.lanes.empty()) {
            std::vector<LaneSpec> selected;
            for (const auto& name : rc.lanes)
                for (const auto& lane : lanes)
                    if (lane.name == name)
                        selected.push_back(lane);
            if (selected.size() != rc.lanes.size()) {
                std::fprintf(stderr, "comparator: unknown lane\n");
                return 2;
            }
            lanes = selected;
        }
#if !PULP_HAS_DAWN_COMPARATOR
        std::fprintf(stderr, "comparator: Dawn provider not built\n");
        return 77;
#else
        struct CellPlan {
            std::size_t lane;
            std::uint32_t frames;
            std::string load;
        };
        std::vector<CellPlan> order;
        for (std::size_t l = 0; l < lanes.size(); ++l)
            for (auto f : rc.frames)
                for (const auto& load : rc.loads)
                    order.push_back({l, f, load});
        // Randomized cell order keeps drift and thermal state from lining up
        // with a lane or a block size.
        std::mt19937 shuffle_rng(rc.seed);
        std::shuffle(order.begin(), order.end(), shuffle_rng);

        std::vector<Workload> workloads;
        for (auto f : rc.frames)
            workloads.push_back(make_workload(f, rc.seed));
        const auto workload_for = [&](std::uint32_t frames) -> const Workload& {
            for (const auto& w : workloads)
                if (w.frames == frames)
                    return w;
            return workloads.front();
        };

        RenderLoad render;
        std::string render_reason;
        bool render_available = false;
        if (std::find(rc.loads.begin(), rc.loads.end(), "render") != rc.loads.end())
            render_available = render.ensure(render_reason);

        const std::function<std::uint64_t()> no_render_cpu = [] { return std::uint64_t{0}; };
        const std::function<std::uint64_t()> render_thread_cpu = [&render] {
            return render.thread_cpu_total();
        };

        DawnIdentity dawn_identity;
        HostClockBridge clock;
        std::vector<CellResult> cells;
        bool any_failure = false;
        for (const auto& plan : order) {
            const auto& spec = lanes[plan.lane];
            const auto& w = workload_for(plan.frames);
            CellResult cell;
            cell.lane = spec.name;
            cell.side = spec.side;
            cell.frames = plan.frames;
            cell.load = plan.load;
            cell.role = spec.role;
            const bool planted_cell = rc.planted && spec.side == "metal";
            if (spec.side == "metal") {
                cell.kernel =
                    planted_cell
                        ? make_kernel_identity(
                              kPlantedProgramId, pipelines.msl_sha256, pipelines.msl_sha256,
                              pipelines.msl_sha256, kRobustnessNone, w,
                              {{"complex_multiply", 0, 0, MetalLane::planted_groups(w)}})
                        : make_kernel_identity(kProgramId, kPortedFromFftWgslSha256,
                                               kPortedFromMulWgslSha256, pipelines.msl_sha256,
                                               spec.guarded ? kRobustnessTint : kRobustnessNone, w,
                                               convolution_passes(w));
                cell.gpu_timestamps = true;
                cell.gpu_busy_domain = "metal_command_buffer_gpu_start_to_end";
                cell.wake_domain = "metal_gpu_end_to_worker_observed";
            } else {
                cell.kernel = make_kernel_identity(
                    kProgramId, wgsl.ok ? wgsl.fft_sha256 : "unavailable",
                    wgsl.ok ? wgsl.mul_sha256 : "unavailable", kPinnedMslSha256, kRobustnessTint, w,
                    convolution_passes(w));
                cell.gpu_busy_domain = "dawn_timestamp_query_first_to_last_compute_pass";
                cell.wake_domain = "unobservable_no_host_completion_time_exposed";
            }
            if (plan.load == "render" && !render_available) {
                cell.status = "unavailable";
                cell.reason = "render_load_unavailable:" + render_reason;
                cells.push_back(std::move(cell));
                continue;
            }
            const std::uint32_t total = rc.warmup + rc.blocks;
            if (plan.load == "render")
                render.start();
            const auto& render_cpu = plan.load == "render" ? render_thread_cpu : no_render_cpu;
            if (spec.side == "metal") {
                MetalLane lane(device, pipelines, spec.metal_completion, spec.guarded,
                               planted_cell);
                if (!lane.prepare(w, total, reason)) {
                    cell.status = "unavailable";
                    cell.reason = reason;
                } else {
                    on_worker_thread([&] { drive(lane, w, rc, cell, render_cpu); });
                    const auto& start = lane.gpu_start();
                    const auto& end = lane.gpu_end();
                    const auto t0 = static_cast<std::int64_t>(cell.t0_ns);
                    for (std::size_t i = 0; i < cell.records.size(); ++i) {
                        const auto k = i + rc.warmup;
                        if (start[k] >= 0 && end[k] >= start[k]) {
                            cell.records[i].gpu_busy_ns = end[k] - start[k];
                            cell.records[i].gpu_end_ns = end[k] - t0;
                        }
                        if (cell.records[i].callback_ns >= 0)
                            cell.records[i].callback_ns -= t0;
                        cell.records[i].callback_cpu_ns = lane.callback_cpu(k);
                    }
                    cell.late_gpu_time_waits = lane.late_gpu_time_waits();
                    if (lane.command_errors() != 0 && cell.status == "completed") {
                        cell.status = "failed";
                        cell.reason = "metal_command_buffer_errors";
                    }
                }
                cell.extra_json = "\"metal\":{\"completion\":" +
                                  json_string(spec.metal_completion == MetalCompletion::SharedEvent
                                                  ? "MTLSharedEvent_notifyListener_semaphore"
                                                  : "addCompletedHandler_semaphore") +
                                  ",\"storage\":\"no_copy_page_aligned_host\",\"robustness\":" +
                                  json_string(spec.guarded ? kRobustnessTint : kRobustnessNone) +
                                  ",\"planted_unmatched_kernel\":" + json_bool(planted_cell) + "}";
            } else {
                const auto policy =
                    spec.dawn_policy == 2   ? DawnSharedIoProvider::CompletionPolicy::TimedWaitAny
                    : spec.dawn_policy == 1 ? DawnSharedIoProvider::CompletionPolicy::WaitAny
                                            : DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
                DawnLane lane(policy, spec.dawn_staged_timestamps, spec.poll_interval_ns);
                if (!lane.prepare(w, total, reason, dawn_identity)) {
                    cell.status = "unavailable";
                    cell.reason = reason;
                } else {
                    cell.gpu_timestamps = lane.timestamps_enabled();
                    on_worker_thread([&] { drive(lane, w, rc, cell, render_cpu); });
                    for (std::size_t i = 0; i < cell.records.size(); ++i)
                        cell.records[i].gpu_busy_ns = lane.gpu_busy()[i + rc.warmup];
                    const auto stats = lane.stats();
                    cell.extra_json =
                        "\"dawn\":{\"completion_policy\":" +
                        json_string(spec.dawn_policy == 2   ? "TimedWaitAny"
                                    : spec.dawn_policy == 1 ? "WaitAny"
                                                            : "ProcessEvents") +
                        ",\"storage\":" +
                        json_string(spec.dawn_staged_timestamps ? "staged"
                                                                : "imported_host_pointer") +
                        ",\"enable_timestamps\":" + json_bool(spec.dawn_staged_timestamps) +
                        ",\"timestamps_unavailable_reason\":" +
                        json_string(spec.dawn_staged_timestamps
                                        ? ""
                                        : "timestamps_require_staged_storage") +
                        ",\"wait_mode\":" +
                        json_string(spec.dawn_policy == 2
                                        ? "timed_wait_any_on_queue_future_else_50us_poll_fallback"
                                        : "poll_then_sleep_poll_interval") +
                        ",\"wait_fallback_sleeps\":" + std::to_string(lane.fallback_sleeps()) +
                        ",\"robustness\":\"dawn_default_tint_robustness\"" +
                        ",\"poll_interval_ns\":" + std::to_string(spec.poll_interval_ns) +
                        ",\"production_eligible\":" + json_bool(spec.production_eligible) +
                        ",\"stats\":{\"service_calls\":" + std::to_string(stats.service_calls) +
                        ",\"process_events_calls\":" + std::to_string(stats.process_events_calls) +
                        ",\"wait_any_calls\":" + std::to_string(stats.wait_any_calls) +
                        ",\"wait_any_timeouts\":" + std::to_string(stats.wait_any_timeouts) +
                        ",\"runtime_write_buffer_calls\":" +
                        std::to_string(stats.runtime_write_buffer_calls) +
                        ",\"timestamp_samples\":" + std::to_string(stats.timestamp_samples) +
                        ",\"timestamp_failures\":" + std::to_string(stats.timestamp_failures) +
                        "}}";
                    if (!lane.release() && cell.status == "completed") {
                        cell.status = "failed";
                        cell.reason = "release_failed";
                    }
                }
            }
            if (plan.load == "render")
                cell.render = render.stop();
            if (cell.status == "failed" || (cell.oracle_failures != 0 && !planted_cell))
                any_failure = true;
            std::fprintf(stderr,
                         "comparator: %-40s frames=%3u load=%-6s %s%s%s oracle_failures=%llu\n",
                         cell.lane.c_str(), cell.frames, cell.load.c_str(), cell.status.c_str(),
                         cell.reason.empty() ? "" : ":", cell.reason.c_str(),
                         static_cast<unsigned long long>(cell.oracle_failures));
            cells.push_back(std::move(cell));
        }

        // Output parity: every completed cell against one Dawn reference lane
        // at the same block size, over the same deterministic patterns.
        std::string parity_json = "[";
        bool parity_ok = true;
        bool planted_detected = false;
        for (const auto frames : rc.frames) {
            const CellResult* reference = nullptr;
            for (const auto& c : cells)
                if (c.frames == frames && c.side == "dawn" && c.status == "completed" &&
                    !c.parity[0].empty() &&
                    (!reference ||
                     std::tie(c.lane, c.load) < std::tie(reference->lane, reference->load)))
                    reference = &c;
            for (const auto& c : cells) {
                if (c.frames != frames || c.status != "completed" || c.parity[0].empty())
                    continue;
                double max_diff = reference ? 0.0 : INFINITY;
                for (std::uint32_t p = 0; reference && p < kPatterns; ++p) {
                    if (c.parity[p].size() != reference->parity[p].size()) {
                        max_diff = INFINITY;
                        break;
                    }
                    for (std::size_t i = 0; i < c.parity[p].size(); ++i)
                        max_diff =
                            std::max(max_diff, std::fabs(static_cast<double>(c.parity[p][i]) -
                                                         reference->parity[p][i]));
                }
                const bool ok = max_diff <= kParityTolerance;
                const bool planted_cell = rc.planted && c.side == "metal";
                if (!ok && !planted_cell)
                    parity_ok = false;
                if (!ok && planted_cell)
                    planted_detected = true;
                if (parity_json.size() > 1)
                    parity_json += ",";
                parity_json +=
                    "{\"frames\":" + std::to_string(frames) + ",\"lane\":" + json_string(c.lane) +
                    ",\"load\":" + json_string(c.load) +
                    ",\"reference_lane\":" + json_string(reference ? reference->lane : "") +
                    ",\"max_abs_diff\":" + json_double(max_diff) +
                    ",\"tolerance\":" + json_double(kParityTolerance) + "}";
            }
        }
        parity_json += "]";

        std::string lanes_json = "[";
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto& l = lanes[i];
            if (i)
                lanes_json += ",";
            lanes_json += "{\"name\":" + json_string(l.name) + ",\"side\":" + json_string(l.side) +
                          ",\"production_eligible\":" + json_bool(l.production_eligible) +
                          ",\"role\":" + json_string(l.role) + "}";
        }
        lanes_json += "]";
        std::string order_json = "[";
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (i)
                order_json += ",";
            order_json += "{\"lane\":" + json_string(lanes[order[i].lane].name) +
                          ",\"frames\":" + std::to_string(order[i].frames) +
                          ",\"load\":" + json_string(order[i].load) + "}";
        }
        order_json += "]";

        std::string out =
            "{\"schema\":" + json_string(kSchema) + ",\"mode\":" + json_string(rc.mode) +
            ",\"planted_unmatched_kernel\":" + json_bool(rc.planted) +
            ",\"device\":{\"metal_name\":" + json_string(device.name.UTF8String) +
            ",\"registry_id\":" + std::to_string(device.registryID) +
            ",\"unified_memory\":" + json_bool(device.hasUnifiedMemory) + "}" +
            ",\"dawn\":{\"expected_dawn_revision\":" +
            json_string(PULP_GPU_AUDIO_EXPECTED_DAWN_SHA) +
            ",\"dawn_revision\":" + json_string(dawn_identity.dawn_revision) +
            ",\"provider_revision\":" + json_string(dawn_identity.provider_revision) +
            ",\"provider_authenticated\":" + json_bool(dawn_identity.authenticated) +
            ",\"native_runtime_authenticated\":" +
            json_bool(dawn_identity.native_runtime_authenticated) +
            ",\"adapter_name\":" + json_string(dawn_identity.adapter_name) +
            ",\"adapter_backend\":" + json_string(dawn_identity.adapter_backend) +
            ",\"native_runtime_name\":" + json_string(dawn_identity.native_runtime_name) +
            ",\"native_runtime_revision\":" + json_string(dawn_identity.native_runtime_revision) +
            ",\"wp_t_switches\":\"not_present_on_base\"}" +
            ",\"kernel\":{\"program\":" + json_string(kProgramId) +
            ",\"provider_wgsl\":{\"ok\":" + json_bool(wgsl.ok) +
            ",\"reason\":" + json_string(wgsl.reason) +
            ",\"source_path\":" + json_string(wgsl.source_path) +
            ",\"source_sha256\":" + json_string(wgsl.source_sha256) +
            ",\"fft_sha256\":" + json_string(wgsl.fft_sha256) +
            ",\"mul_sha256\":" + json_string(wgsl.mul_sha256) +
            ",\"build_fft_sha256\":" + json_string(PULP_COMPARATOR_BUILD_FFT_WGSL_SHA256) +
            ",\"build_mul_sha256\":" + json_string(PULP_COMPARATOR_BUILD_MUL_WGSL_SHA256) + "}" +
            ",\"msl_port\":{\"pinned_msl_sha256\":" + json_string(kPinnedMslSha256) +
            ",\"ported_from_fft_wgsl_sha256\":" + json_string(kPortedFromFftWgslSha256) +
            ",\"ported_from_mul_wgsl_sha256\":" + json_string(kPortedFromMulWgslSha256) +
            ",\"msl_sha256\":" + json_string(pipelines.msl_sha256) +
            ",\"math_mode\":" + json_string(pipelines.math_mode) + "}" +
            ",\"parity\":" + parity_json + "}" +
            ",\"config\":{\"sample_rate\":" + std::to_string(kSampleRate) +
            ",\"channels\":" + std::to_string(kChannels) +
            ",\"flight_depth\":" + std::to_string(kFlightDepth) +
            ",\"lead_blocks\":" + std::to_string(kLeadBlocks) +
            ",\"blocks_per_cell\":" + std::to_string(rc.blocks) +
            ",\"warmup_blocks\":" + std::to_string(rc.warmup) +
            ",\"frames\":" + json_array(rc.frames) + ",\"paced\":true" +
            ",\"seed\":" + std::to_string(rc.seed) +
            ",\"oracle_tolerance\":" + json_double(kOracleTolerance) +
            ",\"worker_qos\":\"QOS_CLASS_USER_INTERACTIVE\"" +
            ",\"host_clock\":\"steady_clock_ns_relative_to_cell_t0\"" +
            ",\"steady_minus_mach_ns\":" + std::to_string(clock.steady_minus_mach_ns) +
            ",\"render_load\":{\"available\":" + json_bool(render_available) +
            ",\"reason\":" + json_string(render_reason) +
            ",\"width\":" + std::to_string(RenderLoad::kWidth) +
            ",\"height\":" + std::to_string(RenderLoad::kHeight) +
            ",\"target_fps\":60,\"device\":\"separate_pulp_ui_dawn_device\"}" +
            ",\"lanes\":" + lanes_json + ",\"cell_order\":" + order_json + "}" + ",\"cells\":[";
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i)
                out += ",";
            out += cell_json(cells[i], rc);
        }
        out += "]}\n";

        if (!rc.output.empty()) {
            std::ofstream file(rc.output, std::ios::binary);
            file << out;
            if (!file) {
                std::fprintf(stderr, "comparator: cannot write %s\n", rc.output.c_str());
                return 1;
            }
        } else {
            std::fwrite(out.data(), 1, out.size(), stdout);
        }

        if (!wgsl.ok) {
            std::fprintf(stderr, "comparator: %s\n", wgsl.reason.c_str());
            return 1;
        }
        if (rc.planted)
            return planted_detected ? 0 : 1;
        const bool built_from_source = wgsl.fft_sha256 == PULP_COMPARATOR_BUILD_FFT_WGSL_SHA256 &&
                                       wgsl.mul_sha256 == PULP_COMPARATOR_BUILD_MUL_WGSL_SHA256;
        const bool port_current = wgsl.fft_sha256 == kPortedFromFftWgslSha256 &&
                                  wgsl.mul_sha256 == kPortedFromMulWgslSha256 &&
                                  pipelines.msl_sha256 == kPinnedMslSha256 && built_from_source;
        if (!built_from_source)
            std::fprintf(stderr, "comparator: provider source WGSL differs from the build\n");
        if (!port_current)
            std::fprintf(stderr, "comparator: MSL port is stale against the provider WGSL\n");
        if (any_failure || !parity_ok || !port_current)
            return 1;
        bool dawn_ran = false;
        for (const auto& c : cells)
            dawn_ran = dawn_ran || (c.side == "dawn" && c.status == "completed");
        return dawn_ran ? 0 : 77;
#endif
    }
}
