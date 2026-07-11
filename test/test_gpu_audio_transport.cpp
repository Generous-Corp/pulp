#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

namespace {

// Test-only node: out = gain * in. Deterministic, no GPU — exercises the
// transport's scheduling, latency, and miss handling.
class GainNode : public GpuAudioNode {
public:
    GainNode(uint32_t channels, uint32_t block, float gain, MissPolicy mp,
             uint32_t latency = 2, bool supports_fallback = true)
        : channels_(channels), block_(block), gain_(gain), mp_(mp),
          latency_(latency), supports_fallback_(supports_fallback) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "gain";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = mp_;
        d.supports_cpu_fallback = supports_fallback_;
        return d;
    }
    bool prepare() override { return true; }
    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       uint32_t n) override {
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* s = in.channel_ptr(c);
            float* d = out.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i) d[i] = s[i] * gain_;
        }
    }

private:
    uint32_t channels_, block_;
    float gain_;
    MissPolicy mp_;
    uint32_t latency_;
    bool supports_fallback_;
};

// Per-channel storage with stable float / const-float pointer arrays.
struct Block {
    Block(uint32_t ch, uint32_t n) : storage(ch, std::vector<float>(n, 0.0f)),
                                     ptrs(ch), cptrs(ch), n(n) {
        for (uint32_t c = 0; c < ch; ++c) { ptrs[c] = storage[c].data(); cptrs[c] = storage[c].data(); }
    }
    void fill(float v) { for (auto& ch : storage) std::fill(ch.begin(), ch.end(), v); }
    BufferView<float> view() { return BufferView<float>(ptrs.data(), ptrs.size(), n); }
    BufferView<const float> cview() { return BufferView<const float>(cptrs.data(), cptrs.size(), n); }
    std::vector<std::vector<float>> storage;
    std::vector<float*> ptrs;
    std::vector<const float*> cptrs;
    uint32_t n;
};

} // namespace

TEST_CASE("GpuAudioTransport applies fixed latency + node processing", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);   // RT: write input, read delayed output
        t.pump();                // worker keeps pace (one block per call)
        first_sample.push_back(out.storage[0][0]);
    }

    for (int k = 0; k < NBLK; ++k) {
        const float expected = (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport miss policy fills dry on starvation", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    for (int k = 0; k < 4; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);   // NEVER pump → worker starved
        first_sample.push_back(out.storage[0][0]);
    }

    REQUIRE(first_sample[0] == 0.0f);
    REQUIRE(first_sample[1] == 0.0f);
    REQUIRE(first_sample[2] == 3.0f);   // dry input value k+1 = 3
    REQUIRE(first_sample[3] == 4.0f);
    REQUIRE(t.stats().miss_blocks == 2);
}

TEST_CASE("GpuAudioTransport process_offline drives the node synchronously", "[gpu_audio][transport]") {
    // Offline render (e.g. a faster-than-real-time bounce): NO worker thread and
    // we NEVER call pump() manually — process_offline() must pump the node inline
    // so every block is captured. This is exactly the case where the realtime
    // process() path starves and misses (see the miss-policy test above); here the
    // miss policy is Silence so any drop would show up as a zero, yet none occur.
    // Latency stays identical to process() (the primed output ring).
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::Silence, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));   // run_worker_thread defaults to false
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process_offline(iv, ov, BS);   // synchronous: no worker, no manual pump
        first_sample.push_back(out.storage[0][0]);
    }

    // Same latency-delayed gain output as the worker-paced realtime case, but with
    // ZERO misses — proving the node ran for every block under offline drive.
    for (int k = 0; k < NBLK; ++k) {
        const float expected = (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport drops input on a full ring (no block)", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, RING = 4;
    GainNode node(CH, BS, 1.0f, MissPolicy::Silence, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    for (int k = 0; k < 20; ++k) {
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
    }
    REQUIRE(t.stats().input_dropped_frames > 0);
}

TEST_CASE("GpuAudioTransport rejects invalid descriptor / config", "[gpu_audio][transport]") {
    // Zero channels.
    {
        GainNode bad(0, 64, 1.0f, MissPolicy::Silence, 2);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&bad, {8}));
        REQUIRE_FALSE(t.is_prepared());
    }
    // ring_blocks too small for latency (needs latency + 2).
    {
        GainNode node(1, 64, 1.0f, MissPolicy::Silence, 4);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {5}));   // 5 < 4 + 2
        REQUIRE(t.prepare(&node, {6}));         // ok
    }
    // CpuFallback miss policy without a real fallback.
    {
        GainNode node(1, 64, 1.0f, MissPolicy::CpuFallback, 2, /*supports_fallback=*/false);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {8}));
    }
    // null node.
    {
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(nullptr, {8}));
    }
}

TEST_CASE("GpuAudioTransport background worker drains the pipeline", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300));  // ~ worker poll pace
    }

    // Liveness: give the worker a bounded grace period to drain the backlog.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0);  // the worker actually ran
    // Exact, timing-independent invariant: every fed block was produced or
    // dropped whole (no partial/misaligned blocks).
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));
    // A resync can only ever cancel a prior miss's late wet block, so the count
    // of resynced blocks can never exceed the misses. A depth-based resync would
    // violate this under worker timing (it drains a block the worker raced ahead
    // to produce even with zero misses); the miss-counting resync cannot.
    REQUIRE(s.resynced_blocks <= s.miss_blocks);

    // release() must cleanly stop + join the worker (no hang / no crash).
    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

TEST_CASE("GpuAudioTransport resyncs the wet timeline after a miss", "[gpu_audio][transport]") {
    // A miss emits a substitute (dry) block for its timeline slot; when the worker
    // later catches up it back-fills that slot's wet block, pushing the output ring
    // above the primed steady-state depth. Without a resync the RT read would pick
    // up that stale wet block, permanently delaying the wet stream one block per
    // miss (a comb filter of dry against a one-block-late wet). This drives a
    // deterministic miss-then-catch-up sequence with manual pump control and proves
    // the resync drops the redundant block so effective latency stays pinned.
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));   // no worker thread — we drive pump()

    Block in(CH, BS), out(CH, BS);
    std::vector<float> got;
    constexpr int NBLK = 8;
    for (int k = 0; k < NBLK; ++k) {
        // Worker catches up BEFORE the callback from k>=6 on: drain the backlog
        // (including the late wet block for the missed slot) so the RT read sees
        // the over-filled ring the resync must correct.
        if (k >= 6) t.pump();
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        // Keep pace through call 2; starve calls 3-5 (drains the 2-block cushion,
        // forcing exactly one miss at call 5); catch-up handled above from k>=6.
        if (k <= 2) t.pump();
        got.push_back(out.storage[0][0]);
    }

    // out[5] is the dry substitute (input 6) — the one block the miss cost us.
    // out[6] is realigned to the no-miss value (10), NOT the stale late wet (8).
    const std::vector<float> expected = {0, 0, 2, 4, 6, /*miss→dry*/6, /*resynced*/10, 12};
    REQUIRE(got == expected);
    REQUIRE(t.stats().miss_blocks == 1);
    REQUIRE(t.stats().resynced_blocks == 1);   // exactly the one late wet block dropped
}

TEST_CASE("GpuAudioTransport wake-on-write worker drains the pipeline", "[gpu_audio][transport]") {
    // Same whole-block conservation invariant as the polling worker, but with the
    // opt-in wake-on-write path: process() posts a semaphore the worker waits on.
    // Proves the semaphore-driven worker still drains every fed block and that
    // release() cleanly joins it.
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true, /*wake_on_write=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0);
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));

    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

TEST_CASE("GpuAudioTransport silences mismatched / too-small views", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    out.fill(7.0f);  // sentinel — must be overwritten with silence

    // Wrong block size.
    auto iv = in.cview();
    auto ov = out.view();
    t.process(iv, ov, BS / 2);
    REQUIRE(out.storage[0][0] == 0.0f);

    // Too few output channels.
    Block mono_out(1, BS);
    mono_out.fill(7.0f);
    auto mov = mono_out.view();
    t.process(iv, mov, BS);
    REQUIRE(mono_out.storage[0][0] == 0.0f);
}

// A node that declares nothing about its miss behavior must not pass audio
// through on a miss. Passing through is the WORST default here: the transport's
// output is delayed by latency_blocks, so a dry sample for the CURRENT time jumps
// the stream forward by the whole latency for one block and back again -- a
// timeline break rather than an honest dropout. And a node that never thought
// about misses is exactly the node whose CPU fallback is not correct.
TEST_CASE("GpuAudioNodeDescriptor defaults fail closed", "[gpu_audio][transport]") {
    GpuAudioNodeDescriptor d;
    REQUIRE(d.miss_policy == MissPolicy::Silence);
    REQUIRE_FALSE(d.supports_cpu_fallback);
}

TEST_CASE("GpuAudioTransport starves to silence when the node declares no policy",
          "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;

    // A node whose descriptor leaves miss_policy and supports_cpu_fallback at
    // their defaults -- the shape a first-time GPU node author writes.
    struct DefaultPolicyNode : GpuAudioNode {
        GpuAudioNodeDescriptor descriptor() const override {
            GpuAudioNodeDescriptor d;   // defaults, deliberately untouched
            d.name = "defaults";
            d.input_channels = CH;
            d.output_channels = CH;
            d.block_size = BS;
            d.sample_rate = 48000;
            d.latency_blocks = L;
            return d;
        }
        bool prepare() override { return true; }
        void process_block(const BufferView<const float>&, BufferView<float>& out,
                           uint32_t n) override {
            for (uint32_t i = 0; i < n; ++i) out.channel_ptr(0)[i] = 1.0f;
        }
    };

    DefaultPolicyNode node;
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    // Never pump the worker, so every read is a miss. The dry input is 0.5 --
    // any nonzero output here is the dry signal leaking through on a miss.
    Block in(CH, BS), out(CH, BS);
    for (int k = 0; k < 4; ++k) {
        in.fill(0.5f);
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        for (uint32_t i = 0; i < BS; ++i) {
            INFO("block " << k << " sample " << i);
            REQUIRE(out.storage[0][i] == 0.0f);
        }
    }
    // Misses were recorded (the exact count depends on the transport's priming
    // and resync bookkeeping; what this test pins is that they came out SILENT).
    REQUIRE(t.stats().miss_blocks >= 1);
}
