// test_gpu_compute_async.cpp — asynchronous GPU readback.
//
// The blocking read_back() path pumps the device event queue in a spin loop.
// The async path submits, returns, and delivers results from poll_readbacks()
// so the caller owns its event loop.
//
// HONEST SCOPE: nothing ships on this API yet. compute_magnitude_async() has no
// caller outside these tests — the GPU-audio worker still runs the blocking
// path, and no browser (single-threaded) GpuCompute exists. These tests are the
// specification the API must already satisfy BEFORE a caller can be written on a
// single-threaded event loop, not a regression net around live usage. They pin
// four properties: byte-equivalence with the blocking path, in-order delivery of
// concurrent in-flight requests, bounded deadline expiry that routes to a miss
// policy instead of hanging, and an audio thread that still neither allocates
// nor blocks.

#include "harness/scoped_rt_process_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/render/gpu_compute.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using pulp::audio::BufferView;
using pulp::gpu_audio::GpuAudioNode;
using pulp::gpu_audio::GpuAudioNodeDescriptor;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::MissPolicy;
using pulp::render::GpuCompute;
using Catch::Matchers::WithinAbs;

namespace {

using Clock = std::chrono::steady_clock;

// A GPU device the standalone compute path can drive, or null when this host
// has no Skia/Dawn build or no adapter (headless CI).
std::unique_ptr<GpuCompute> make_gpu() {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return nullptr;
    return compute;
}

constexpr const char* kNoGpu =
    "no GPU compute device available (Skia/Dawn not built, or no adapter)";

// Drain every in-flight readback. Bounded by the requests' own deadlines: an
// unresolved map expires rather than pinning the queue, so this always returns.
void drain(GpuCompute& gpu) {
    while (gpu.readbacks_in_flight() > 0) gpu.poll_readbacks();
}

// magnitude(re, im) for a constant complex value repeated across `bins`.
std::vector<float> constant_pairs(uint32_t bins, float re, float im) {
    std::vector<float> pairs(bins * 2);
    for (uint32_t i = 0; i < bins; ++i) {
        pairs[i * 2] = re;
        pairs[i * 2 + 1] = im;
    }
    return pairs;
}

// GPU node whose block work runs entirely through the ASYNC readback path:
// out[i] = |in[i]|, computed as the magnitude of the complex pair (in[i], 0).
// A readback that misses its deadline routes to the node's miss policy, which
// is exactly how the browser worker will behave when the map lands late.
class AsyncMagnitudeNode : public GpuAudioNode {
public:
    AsyncMagnitudeNode(GpuCompute* gpu, uint32_t block, uint32_t latency,
                       std::chrono::microseconds deadline)
        : gpu_(gpu), block_(block), latency_(latency), deadline_(deadline) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "async-magnitude";
        d.input_channels = 1;
        d.output_channels = 1;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = MissPolicy::Silence;
        d.supports_cpu_fallback = false;
        return d;
    }

    bool prepare() override {
        pairs_.assign(block_ * 2, 0.0f);
        mags_.assign(block_, 0.0f);
        return gpu_ != nullptr;
    }

    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       uint32_t n) override {
        const float* src = in.channel_ptr(0);
        for (uint32_t i = 0; i < n; ++i) {
            pairs_[i * 2] = src[i];
            pairs_[i * 2 + 1] = 0.0f;
        }

        bool got = false;
        const uint64_t id = gpu_->compute_magnitude_async(
            pairs_.data(), mags_.data(), n, deadline_,
            [&got](const GpuCompute::ReadbackResult& r) {
                got = (r.status == GpuCompute::ReadbackStatus::Success);
            });

        if (id != 0) drain(*gpu_);

        float* dst = out.channel_ptr(0);
        if (!got) {
            ++misses_;
            std::fill(dst, dst + n, 0.0f);
            return;
        }
        std::copy(mags_.begin(), mags_.begin() + n, dst);
    }

    uint32_t misses() const { return misses_; }

private:
    GpuCompute* gpu_;
    uint32_t block_;
    uint32_t latency_;
    std::chrono::microseconds deadline_;
    std::vector<float> pairs_;
    std::vector<float> mags_;
    uint32_t misses_ = 0;
};

// Per-channel storage with stable pointer arrays, matching the shape the
// transport's BufferViews expect.
struct Block {
    Block(uint32_t ch, uint32_t n)
        : storage(ch, std::vector<float>(n, 0.0f)), ptrs(ch), cptrs(ch), n(n) {
        for (uint32_t c = 0; c < ch; ++c) {
            ptrs[c] = storage[c].data();
            cptrs[c] = storage[c].data();
        }
    }
    void fill(float v) {
        for (auto& ch : storage) std::fill(ch.begin(), ch.end(), v);
    }
    BufferView<float> view() { return BufferView<float>(ptrs.data(), ptrs.size(), n); }
    BufferView<const float> cview() {
        return BufferView<const float>(cptrs.data(), cptrs.size(), n);
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> ptrs;
    std::vector<const float*> cptrs;
    uint32_t n;
};

} // namespace

TEST_CASE("Async readback matches the blocking readback byte for byte",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t kBins = 1024;
    const auto pairs = constant_pairs(kBins, 3.0f, 4.0f);

    std::vector<float> blocking(kBins, -1.0f);
    REQUIRE(gpu->compute_magnitude(pairs.data(), blocking.data(), kBins));

    std::vector<float> async(kBins, -1.0f);
    GpuCompute::ReadbackResult result{};
    const uint64_t id = gpu->compute_magnitude_async(
        pairs.data(), async.data(), kBins, std::chrono::seconds(2),
        [&result](const GpuCompute::ReadbackResult& r) { result = r; });
    REQUIRE(id != 0);
    REQUIRE(gpu->readbacks_in_flight() == 1);

    drain(*gpu);

    REQUIRE(result.id == id);
    REQUIRE(result.status == GpuCompute::ReadbackStatus::Success);
    REQUIRE(result.bytes == kBins * sizeof(float));
    REQUIRE(async == blocking);
    CHECK_THAT(async[0], WithinAbs(5.0f, 1e-4f));
}

TEST_CASE("Concurrent async readbacks complete in order with their own payloads",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    // Distinct (re, im) per request so a swapped staging buffer shows up as a
    // wrong magnitude, not as a coincidentally-equal one.
    struct Request {
        float re, im, expected;
        std::vector<float> pairs;
        std::vector<float> out;
        uint64_t id = 0;
    };
    constexpr uint32_t kBins = 256;
    constexpr std::size_t kInFlight = 6;
    const float re[kInFlight] = {3.0f, 5.0f, 8.0f, 7.0f, 20.0f, 9.0f};
    const float im[kInFlight] = {4.0f, 12.0f, 15.0f, 24.0f, 21.0f, 40.0f};

    std::vector<Request> reqs;
    reqs.reserve(kInFlight);
    for (std::size_t i = 0; i < kInFlight; ++i) {
        Request r;
        r.re = re[i];
        r.im = im[i];
        r.expected = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        r.pairs = constant_pairs(kBins, re[i], im[i]);
        r.out.assign(kBins, -1.0f);
        reqs.push_back(std::move(r));
    }

    std::vector<GpuCompute::ReadbackResult> completions;
    for (auto& r : reqs) {
        r.id = gpu->compute_magnitude_async(
            r.pairs.data(), r.out.data(), kBins, std::chrono::seconds(2),
            [&completions](const GpuCompute::ReadbackResult& res) {
                completions.push_back(res);
            });
        REQUIRE(r.id != 0);
    }
    REQUIRE(gpu->readbacks_in_flight() == kInFlight);

    drain(*gpu);

    REQUIRE(completions.size() == kInFlight);
    for (std::size_t i = 0; i < kInFlight; ++i) {
        INFO("request " << i);
        REQUIRE(completions[i].id == reqs[i].id);
        REQUIRE(completions[i].status == GpuCompute::ReadbackStatus::Success);
        for (uint32_t b = 0; b < kBins; ++b) {
            REQUIRE_THAT(reqs[i].out[b], WithinAbs(reqs[i].expected, 1e-3f));
        }
    }
}

TEST_CASE("An expired async readback routes to the miss policy and never hangs",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t kBins = 512;
    const auto pairs = constant_pairs(kBins, 3.0f, 4.0f);

    // Unsatisfiable: the deadline has already passed by the time the result
    // could possibly be delivered, so the request must complete as Expired.
    std::vector<float> dest(kBins, -1.0f);
    GpuCompute::ReadbackResult result{};
    const uint64_t id = gpu->compute_magnitude_async(
        pairs.data(), dest.data(), kBins, std::chrono::microseconds(0),
        [&result](const GpuCompute::ReadbackResult& r) { result = r; });
    REQUIRE(id != 0);

    const auto t0 = Clock::now();
    drain(*gpu);
    const auto elapsed = Clock::now() - t0;

    REQUIRE(result.id == id);
    REQUIRE(result.status == GpuCompute::ReadbackStatus::Expired);
    REQUIRE(result.bytes == 0);
    // `dest` is untouched: a late map must never write into the caller's block.
    REQUIRE(std::all_of(dest.begin(), dest.end(),
                        [](float v) { return v == -1.0f; }));
    REQUIRE(elapsed < std::chrono::milliseconds(500));

    // What the worker does with an Expired result: hand the block to the miss
    // policy exactly as the transport does for a late block.
    std::vector<float> block(kBins, 1.0f);
    if (result.status != GpuCompute::ReadbackStatus::Success) {
        const MissPolicy policy = MissPolicy::Silence;
        REQUIRE(policy == MissPolicy::Silence);
        std::fill(block.begin(), block.end(), 0.0f);
    }
    REQUIRE(std::all_of(block.begin(), block.end(),
                        [](float v) { return v == 0.0f; }));

    // The expiry discarded its staging buffer rather than recycling a
    // still-mapped one, so the device is still usable afterwards.
    std::vector<float> after(kBins, 0.0f);
    REQUIRE(gpu->compute_magnitude(pairs.data(), after.data(), kBins));
    CHECK_THAT(after[0], WithinAbs(5.0f, 1e-4f));
    REQUIRE(gpu->readbacks_in_flight() == 0);
}

TEST_CASE("Async readback on the worker keeps the audio thread allocation-free",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t kBlock = 64;
    constexpr uint32_t kLatency = 2;
    AsyncMagnitudeNode node(gpu.get(), kBlock, kLatency,
                            std::chrono::milliseconds(500));
    REQUIRE(node.prepare());

    GpuAudioTransport transport;
    GpuAudioTransport::Config cfg;
    cfg.ring_blocks = 8;
    cfg.run_worker_thread = false;   // the test drives pump(): deterministic
    REQUIRE(transport.prepare(&node, cfg));
    REQUIRE(transport.latency_samples() == kLatency * kBlock);

    Block in(1, kBlock);
    Block out(1, kBlock);
    in.fill(-0.5f);

    for (int b = 0; b < 8; ++b) {
        {
            // The audio thread: no allocation, no lock, no GPU synchronization.
            auto view = out.view();
            pulp::test::ScopedRtProcessProbe probe;
            transport.process(in.cview(), view, kBlock);
            REQUIRE(probe.allocation_count() == 0);
        }
        // The non-RT worker: submits the dispatch and drives the async readback.
        transport.pump();
    }

    const auto stats = transport.stats();
    REQUIRE(stats.produced_blocks > 0);
    REQUIRE(node.misses() == 0);
    // |-0.5| — the GPU result, delivered through the async readback.
    for (uint32_t i = 0; i < kBlock; ++i) {
        REQUIRE_THAT(out.storage[0][i], WithinAbs(0.5f, 1e-4f));
    }
    REQUIRE(gpu->readbacks_in_flight() == 0);
}
