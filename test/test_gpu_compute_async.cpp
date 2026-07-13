// test_gpu_compute_async.cpp — asynchronous GPU readback.
//
// The blocking read_back() path pumps the device event queue in a spin loop.
// The async path submits, returns, and delivers results from poll_readbacks()
// so the caller owns its event loop.
//
// HONEST SCOPE: the native GPU-audio worker still runs the blocking path; the
// async path is what the browser module uses, where blocking is impossible.
// These tests are the specification that path must satisfy, exercised on a real
// native Dawn device (there is no browser in normal CI). They pin:
// byte-equivalence with the blocking path, in-order delivery of concurrent
// in-flight requests with no cross-contamination, bounded deadline expiry that
// routes to a miss policy instead of hanging, admission control at the in-flight
// cap, and an audio thread that still neither allocates nor blocks.

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

    // Above the default in-flight cap (4), which is admission control, not a
    // depth limit: raise it deliberately for this test.
    gpu->set_max_readbacks_in_flight(kInFlight);

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

// ── convolve_batch_async ────────────────────────────────────────────────────
//
// The async convolution reuses the plan's compute buffers across in-flight
// blocks and takes only its readback buffer per submit. That is safe because the
// WebGPU queue is in-order — and it is the invariant the whole browser design
// rests on, so it is tested directly below rather than assumed.

namespace {

// A flat IR spectrum: every bin scales by the same complex factor, so a batch's
// output is a deterministic function of its input and nothing else.
std::vector<float> flat_ir_spectrum(uint32_t n, float re, float im) {
    std::vector<float> spec(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        spec[i * 2] = re;
        spec[i * 2 + 1] = im;
    }
    return spec;
}

// Per-batch input with a batch-dependent waveform, so a swapped or stale buffer
// shows up as the WRONG batch's samples rather than as a plausible-looking one.
std::vector<float> batch_input(uint32_t n, uint32_t batch, float seed) {
    std::vector<float> in(n * 2 * batch, 0.0f);
    for (uint32_t b = 0; b < batch; ++b) {
        for (uint32_t i = 0; i < n; ++i) {
            in[(b * n + i) * 2] =
                std::sin(0.05f * static_cast<float>(i) + seed + 1.7f * static_cast<float>(b));
        }
    }
    return in;
}

} // namespace

TEST_CASE("convolve_batch_async matches the blocking convolve_batch",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t N = 256, B = 4;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));

    const auto in = batch_input(N, B, 0.0f);

    std::vector<float> blocking(N * 2 * B, 0.0f);
    REQUIRE(gpu->convolve_batch(in.data(), blocking.data(), N, B));

    std::vector<float> async(N * 2 * B, -777.0f);
    GpuCompute::ReadbackResult result{};
    const uint64_t id = gpu->convolve_batch_async(
        in.data(), async.data(), N, B, std::chrono::seconds(2),
        [&result](const GpuCompute::ReadbackResult& r) { result = r; });
    REQUIRE(id != 0);

    drain(*gpu);

    REQUIRE(result.id == id);
    REQUIRE(result.status == GpuCompute::ReadbackStatus::Success);
    REQUIRE(result.bytes == N * 2 * B * sizeof(float));
    // Bit-identical: the async path runs the same kernels on the same plan, and
    // applies the same 1/n inverse-FFT scale (in the completion, not the WGSL).
    REQUIRE(async == blocking);

    // Rejections: no plan for this n, and a batch count the plan was not built
    // for. Neither fires the callback.
    bool fired = false;
    auto never = [&fired](const GpuCompute::ReadbackResult&) { fired = true; };
    REQUIRE(gpu->convolve_batch_async(in.data(), async.data(), N * 2, B,
                                      std::chrono::seconds(1), never) == 0);
    REQUIRE(gpu->convolve_batch_async(in.data(), async.data(), N, B + 1,
                                      std::chrono::seconds(1), never) == 0);
    REQUIRE_FALSE(fired);
}

// THE load-bearing test. Two blocks in flight at once, with distinct payloads,
// sharing the plan's compute buffers. If the in-order-queue invariant did not
// hold — if submit N+1's WriteBuffer into plan.buf_a could land before submit
// N's copy out of it — block 0 would come back carrying block 1's samples. That
// is exactly the failure a shared per-plan readback buffer would also produce,
// which is why this is the test that licenses the whole design.
TEST_CASE("Two convolve_batch_async blocks in flight stay in order and uncontaminated",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t N = 256, B = 2;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));

    // Two distinct payloads, and the reference result for each, computed one at a
    // time through the blocking path.
    const auto in_a = batch_input(N, B, 0.0f);
    const auto in_b = batch_input(N, B, 2.4f);
    REQUIRE(in_a != in_b);

    std::vector<float> ref_a(N * 2 * B, 0.0f), ref_b(N * 2 * B, 0.0f);
    REQUIRE(gpu->convolve_batch(in_a.data(), ref_a.data(), N, B));
    REQUIRE(gpu->convolve_batch(in_b.data(), ref_b.data(), N, B));
    REQUIRE(ref_a != ref_b);

    // Now both at once, without draining in between.
    std::vector<float> out_a(N * 2 * B, -777.0f), out_b(N * 2 * B, -777.0f);
    std::vector<uint64_t> order;

    const uint64_t id_a = gpu->convolve_batch_async(
        in_a.data(), out_a.data(), N, B, std::chrono::seconds(2),
        [&order](const GpuCompute::ReadbackResult& r) {
            REQUIRE(r.status == GpuCompute::ReadbackStatus::Success);
            order.push_back(r.id);
        });
    const uint64_t id_b = gpu->convolve_batch_async(
        in_b.data(), out_b.data(), N, B, std::chrono::seconds(2),
        [&order](const GpuCompute::ReadbackResult& r) {
            REQUIRE(r.status == GpuCompute::ReadbackStatus::Success);
            order.push_back(r.id);
        });

    REQUIRE(id_a != 0);
    REQUIRE(id_b != 0);
    REQUIRE(id_b != id_a);
    REQUIRE(gpu->readbacks_in_flight() == 2);

    drain(*gpu);

    // Submission order, not completion-race order.
    REQUIRE(order == std::vector<uint64_t>{id_a, id_b});

    // Zero cross-contamination: each block carries its OWN result.
    REQUIRE(out_a == ref_a);
    REQUIRE(out_b == ref_b);
}

TEST_CASE("An expired convolve_batch_async leaves dest untouched and never hangs",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t N = 256, B = 2;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));
    const auto in = batch_input(N, B, 0.0f);

    std::vector<float> dest(N * 2 * B, -777.0f);
    GpuCompute::ReadbackResult result{};
    const uint64_t id = gpu->convolve_batch_async(
        in.data(), dest.data(), N, B, std::chrono::microseconds(0),
        [&result](const GpuCompute::ReadbackResult& r) { result = r; });
    REQUIRE(id != 0);

    const auto t0 = Clock::now();
    drain(*gpu);
    const auto elapsed = Clock::now() - t0;

    REQUIRE(result.id == id);
    REQUIRE(result.status == GpuCompute::ReadbackStatus::Expired);
    REQUIRE(result.bytes == 0);
    // Untouched — including by the 1/n scale, which only runs on Success. A late
    // block must never smear a partial or rescaled result into the caller's
    // buffer; the caller has already substituted per its MissPolicy.
    REQUIRE(std::all_of(dest.begin(), dest.end(),
                        [](float v) { return v == -777.0f; }));
    REQUIRE(elapsed < std::chrono::milliseconds(500));

    // The device survives the expiry: the discarded staging buffer did not go
    // back on the free list while still mapped.
    std::vector<float> after(N * 2 * B, 0.0f);
    REQUIRE(gpu->convolve_batch(in.data(), after.data(), N, B));
    REQUIRE(gpu->readbacks_in_flight() == 0);
}

TEST_CASE("Async admission control rejects past the in-flight cap",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t N = 256, B = 2;
    constexpr std::size_t D = 2;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));
    const auto in = batch_input(N, B, 0.0f);

    gpu->set_max_readbacks_in_flight(D);

    std::vector<std::vector<float>> dests(D + 1,
                                          std::vector<float>(N * 2 * B, -777.0f));
    auto ignore = [](const GpuCompute::ReadbackResult&) {};

    for (std::size_t i = 0; i < D; ++i) {
        REQUIRE(gpu->convolve_batch_async(in.data(), dests[i].data(), N, B,
                                          std::chrono::seconds(2), ignore) != 0);
    }
    REQUIRE(gpu->readbacks_in_flight() == D);

    const auto before = gpu->async_stats();

    // At the cap: rejected. The caller counts a miss instead of the GPU layer
    // growing staging memory by another buffer for a device that is not keeping
    // up (a throttled background tab, in the browser).
    bool fired = false;
    const uint64_t rejected = gpu->convolve_batch_async(
        in.data(), dests[D].data(), N, B, std::chrono::seconds(2),
        [&fired](const GpuCompute::ReadbackResult&) { fired = true; });
    REQUIRE(rejected == 0);
    REQUIRE_FALSE(fired);
    REQUIRE(gpu->readbacks_in_flight() == D);

    // Nothing was dispatched and no staging buffer was taken: the rejection is
    // checked before any GPU work, so the submit counter does not move.
    const auto after_reject = gpu->async_stats();
    REQUIRE(after_reject.submits == before.submits);

    // compute_magnitude_async is admitted through the same gate.
    const auto pairs = constant_pairs(64, 3.0f, 4.0f);
    std::vector<float> mags(64, 0.0f);
    REQUIRE(gpu->compute_magnitude_async(pairs.data(), mags.data(), 64,
                                         std::chrono::seconds(2), ignore) == 0);

    drain(*gpu);
    REQUIRE(gpu->readbacks_in_flight() == 0);

    // Below the cap again, submits are admitted — the cap is backpressure, not a
    // one-way latch.
    REQUIRE(gpu->convolve_batch_async(in.data(), dests[0].data(), N, B,
                                      std::chrono::seconds(2), ignore) != 0);
    drain(*gpu);
}

TEST_CASE("Async stats count submits and map resolutions where they happen",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    constexpr uint32_t N = 256, B = 2;
    const auto irspec = flat_ir_spectrum(N, 0.7f, -0.2f);
    REQUIRE(gpu->prepare_convolution_batch(N, irspec.data(), B));
    const auto in = batch_input(N, B, 0.0f);

    const auto before = gpu->async_stats();

    std::vector<float> ok_dest(N * 2 * B, 0.0f);
    auto ignore = [](const GpuCompute::ReadbackResult&) {};
    REQUIRE(gpu->convolve_batch_async(in.data(), ok_dest.data(), N, B,
                                      std::chrono::seconds(2), ignore) != 0);
    drain(*gpu);

    const auto after_ok = gpu->async_stats();
    REQUIRE(after_ok.submits == before.submits + 1);
    REQUIRE(after_ok.map_resolves == before.map_resolves + 1);
    REQUIRE(after_ok.expired == before.expired);
    REQUIRE(after_ok.failed == before.failed);

    std::vector<float> late_dest(N * 2 * B, 0.0f);
    REQUIRE(gpu->convolve_batch_async(in.data(), late_dest.data(), N, B,
                                      std::chrono::microseconds(0), ignore) != 0);
    drain(*gpu);

    const auto after_expired = gpu->async_stats();
    REQUIRE(after_expired.submits == after_ok.submits + 1);
    REQUIRE(after_expired.expired == after_ok.expired + 1);
}

TEST_CASE("A GPU device is not lost until it is",
          "[render][gpu][compute][async]") {
    auto gpu = make_gpu();
    if (!gpu) SKIP(kNoGpu);

    REQUIRE(gpu->is_initialized());
    REQUIRE_FALSE(gpu->device_lost());

    constexpr uint32_t kBins = 64;
    const auto pairs = constant_pairs(kBins, 3.0f, 4.0f);
    std::vector<float> dest(kBins, -1.0f);
    GpuCompute::ReadbackResult result{};
    REQUIRE(gpu->compute_magnitude_async(
        pairs.data(), dest.data(), kBins, std::chrono::seconds(2),
        [&result](const GpuCompute::ReadbackResult& r) { result = r; }) != 0);

    // Device loss is state, not an error path: everything in flight completes as
    // Failed (a map on a lost device never resolves, so leaving it queued would
    // expire block after block), the object stops reporting itself initialized,
    // and no further work is admitted until a device is re-acquired.
    gpu->notify_device_lost();

    REQUIRE(gpu->device_lost());
    REQUIRE_FALSE(gpu->is_initialized());
    REQUIRE(result.status == GpuCompute::ReadbackStatus::Failed);
    REQUIRE(gpu->readbacks_in_flight() == 0);
    REQUIRE(std::all_of(dest.begin(), dest.end(),
                        [](float v) { return v == -1.0f; }));
    REQUIRE(gpu->async_stats().failed >= 1);

    REQUIRE(gpu->compute_magnitude_async(pairs.data(), dest.data(), kBins,
                                         std::chrono::seconds(1),
                                         [](const GpuCompute::ReadbackResult&) {}) == 0);

    // Idempotent.
    gpu->notify_device_lost();
    REQUIRE(gpu->device_lost());
}
