// SuperConvolver's BROWSER GPU engine, tested natively with no browser and no
// GPU device.
//
// The engine reaches its "GPU" through exactly ONE seam — the `pulp_gpu_xfer`
// wasm import — so a native build that compiles the processor with PULP_WASM +
// PULP_WEB_GPU_AUDIO and links its own pulp_gpu_xfer runs the same engine the
// browser runs. What it does NOT get for free is the browser's ring: the import
// is a two-line function, and a stub that always answers in lockstep with the
// plugin's calls would model a transport that cannot drop an input, cannot expire
// a block, and cannot hold un-consumed output across an Engine flip — which are
// exactly the three states the engine can get wrong.
//
// So the stub here is a FAITHFUL PORT of the real transport
// (examples/web-demos/gpu-audio/js/gpu-ring.mjs): bounded slots, a worker that
// runs between audio blocks and can fall behind, seq-stamped blocks, and a full
// input ring that DROPS. Every scenario below drives the same engine the browser
// drives, through the same ring semantics.
//
// The load-bearing invariant, asserted from several directions:
//
//   pop() delivers the GPU wet of INPUT block n-L, or nothing. The CPU safety net
//   is delayed by the same L blocks (gpu_extra_ / cpu_extra_ring_), so the two
//   wets are the SAME wet and are interchangeable sample-for-sample on any block.
//   A miss is therefore inaudible, and an Engine flip is glitch-free — in EITHER
//   direction, at ANY time, including after a worker stall that dropped inputs.
//
// If the L bookkeeping is off by even one block, the "always miss" test stops
// matching the CPU engine sample-for-sample and this file goes red. (Verified by
// mutation: shortening gpu_extra_ by one block turns 3 of these cases red.)
//
// The companion target (same source, WITHOUT PULP_WEB_GPU_AUDIO) pins the other
// half of the contract: the shipped CPU-only web module still declares exactly
// its four parameters and grows no GPU controls.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/state/store.hpp>

#include "super_convolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

using namespace pulp::examples;

namespace {

constexpr double kSr = 48000.0;
constexpr std::size_t kBlock = SuperConvolverProcessor::kInternalBlock;   // 512
constexpr float kIrSeconds = 0.5f;   // lands exactly on the Size quantizer's grid

pulp::format::PrepareContext prep_ctx(std::size_t block) {
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kSr;
    ctx.max_buffer_size = static_cast<int>(block);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    return ctx;
}

}  // namespace

#if defined(PULP_WEB_GPU_AUDIO)

namespace {

constexpr std::size_t kLat = SuperConvolverProcessor::kWebGpuLatencyBlocks;  // 2
constexpr std::size_t kSlots = 4;          // the page's default (gpu-bridge.mjs)
constexpr std::size_t kChan = 2;
constexpr std::size_t kBlockFloats = kChan * kBlock;

// A deterministic, broadband, non-repeating stimulus — a repeating waveform could
// hide a one-block misalignment behind its own period.
float stimulus(std::size_t i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    const std::uint32_t s = static_cast<std::uint32_t>(i) * 1664525u + 1013904223u;
    const float noise = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
    return 0.5f * std::sin(6.2831853f * 220.0f * t) + 0.25f * noise;
}

// ── The stub browser: a faithful port of the SAB ring ────────────────────────
// gpu-ring.mjs, in C++. Two bounded rings of `kSlots` blocks; the worklet side
// (push/pop) is what the plugin drives through pulp_gpu_xfer, the worker side
// (take/publish) is what the test's stand-in GPU worker drives. Every block
// carries the worklet's monotonic BLOCK INDEX, and pop() asks for the one block it
// is owed — which is why a dropped input or an unpublished (expired) block costs
// exactly one MISS instead of permanently shifting the wet stream.
class RingModel {
public:
    // ── worklet (audio thread) ──
    bool push(const float* src) {
        if (in_w_ - in_r_ >= kSlots) { ++dropped; return false; }
        const std::size_t slot = in_w_ % kSlots;
        std::memcpy(in_[slot].data(), src, kBlockFloats * sizeof(float));
        in_seq_[slot] = block_index_;
        ++in_w_;
        return true;
    }

    bool pop(float* dst) {
        const std::int64_t want = block_index_ - static_cast<std::int64_t>(kLat);
        ++block_index_;
        bool hit = false;
        while (out_w_ > out_r_) {
            const std::size_t slot = out_r_ % kSlots;
            const std::int64_t age = out_seq_[slot] - want;
            if (age > 0) break;                       // never produced → miss
            ++out_r_;
            if (age < 0) { ++resynced; continue; }    // a late wet already covered
            std::memcpy(dst, out_[slot].data(), kBlockFloats * sizeof(float));
            hit = true;
            break;
        }
        if (!hit) ++missed;
        return hit;
    }

    // ── worker ──
    bool take(float* dst, std::int64_t& seq) {
        if (in_w_ == in_r_) return false;
        const std::size_t slot = in_r_ % kSlots;
        std::memcpy(dst, in_[slot].data(), kBlockFloats * sizeof(float));
        seq = in_seq_[slot];
        ++in_r_;
        return true;
    }

    bool publish(const float* src, std::int64_t seq) {
        if (out_w_ - out_r_ >= kSlots) return false;
        const std::size_t slot = out_w_ % kSlots;
        std::memcpy(out_[slot].data(), src, kBlockFloats * sizeof(float));
        out_seq_[slot] = seq;
        ++out_w_;
        return true;
    }

    bool output_full() const { return out_w_ - out_r_ >= kSlots; }

    std::uint64_t dropped = 0, missed = 0, resynced = 0;

private:
    std::array<std::vector<float>, kSlots> in_{}, out_{};
    std::array<std::int64_t, kSlots> in_seq_{}, out_seq_{};
    std::size_t in_w_ = 0, in_r_ = 0, out_w_ = 0, out_r_ = 0;
    std::int64_t block_index_ = 0;

public:
    RingModel() {
        for (auto& b : in_) b.assign(kBlockFloats, 0.0f);
        for (auto& b : out_) b.assign(kBlockFloats, 0.0f);
    }
};

// The stand-in for the WebGPU worker. It convolves with the same IR the plugin's
// own CPU engine holds, so its wet IS the plugin's CPU wet (to ~1 ULP — see
// kSeamTol) and "a hit and a miss produce the same audio" becomes a sample-wise
// equality a test can assert. It is driven explicitly between audio blocks, so a
// test can stall it, make it skip a block (an expiry), or run it flat out.
class GpuWorkerStub {
public:
    explicit GpuWorkerStub(RingModel& ring, const std::vector<float>& ir) : ring_(ring) {
        for (auto& c : conv_) c.load_ir(ir.data(), ir.size(), kBlock);
        in_.assign(kBlockFloats, 0.0f);
        wet_.assign(kBlockFloats, 0.0f);
        scratch_.assign(kBlock, 0.0f);
    }

    /// One worker pass: consume up to `max_blocks` inputs and convolve each — the
    /// convolvers must see EVERY block, because an overlap-save convolver's output
    /// depends on its input history — then, if `publish`, flush what it holds to the
    /// ring in seq order. `publish = false` models a worker that is running but LATE
    /// (its wets land a pass after the worklet wanted them); `skip_seq` models a
    /// block whose deadline EXPIRED (consumed, convolved, never published).
    void run(int max_blocks = 8, bool publish = true) {
        for (int i = 0; i < max_blocks; ++i) {
            std::int64_t seq = 0;
            if (!ring_.take(in_.data(), seq)) break;
            for (std::size_t ch = 0; ch < kChan; ++ch) {
                conv_[ch].process(in_.data() + ch * kBlock, scratch_.data(), kBlock);
                std::memcpy(wet_.data() + ch * kBlock, scratch_.data(), kBlock * sizeof(float));
            }
            if (seq == skip_seq) { ++expired; continue; }   // deadline expired
            pending_.push_back({wet_, seq});
        }
        if (!publish) return;
        while (!pending_.empty() && !ring_.output_full()) {
            ring_.publish(pending_.front().wet.data(), pending_.front().seq);
            pending_.erase(pending_.begin());
            ++produced;
        }
    }

    std::int64_t skip_seq = -1;
    std::uint64_t produced = 0, expired = 0;

private:
    struct Held { std::vector<float> wet; std::int64_t seq; };
    RingModel& ring_;
    std::array<pulp::signal::PartitionedConvolver, kChan> conv_{};
    std::vector<float> in_, wet_, scratch_;
    std::vector<Held> pending_;
};

// A worker that does no convolution at all: it publishes a CONSTANT-valued wet
// that identifies the input block it came from. Convolution history is exactly
// what makes the drop case hard to assert on (a dropped input leaves a real
// convolver's overlap-save tail wrong for one IR length — physics, not a bug), so
// the tests that force drops assert PLACEMENT with these markers instead.
float marker(std::int64_t seq) { return 0.01f * static_cast<float>(seq + 1); }

class MarkerWorker {
public:
    explicit MarkerWorker(RingModel& ring) : ring_(ring) {
        in_.assign(kBlockFloats, 0.0f);
        wet_.assign(kBlockFloats, 0.0f);
    }
    void run() {
        std::int64_t seq = 0;
        while (!ring_.output_full() && ring_.take(in_.data(), seq)) {
            std::fill(wet_.begin(), wet_.end(), marker(seq));
            ring_.publish(wet_.data(), seq);
            ++produced;
        }
    }
    std::uint64_t produced = 0;

private:
    RingModel& ring_;
    std::vector<float> in_, wet_;
};

// The one seam. `g_ring` is the transport; `g_pump` is what the "worker" does
// between audio blocks (null = a worker that never runs at all: every block
// misses).
struct Seam {
    RingModel* ring = nullptr;
    std::function<void(std::uint64_t call)> pump;   // runs BEFORE the block is handed over
    std::uint64_t calls = 0, hits = 0, rejected = 0;
    void reset() { ring = nullptr; pump = nullptr; calls = 0; hits = 0; rejected = 0; }
};

Seam g_seam;

// The base IR the processor builds for `seconds` with the default built-in room —
// the same call build_base_ir() makes, so the stub worker and the plugin's CPU
// engine convolve against an identical IR.
std::vector<float> base_ir_for(float seconds) {
    const auto len = static_cast<std::size_t>(seconds * static_cast<float>(kSr));
    return make_builtin_ir(0, len < 1 ? 1 : len);
}

struct Settings {
    float mix = 50.0f;
    int engine = 1;      // 1 = GPU
    int gpu_only = 0;
};

// Prepare a processor with `s` and render `nblocks` host blocks of the stimulus.
// `on_block` (optional) runs before each host block — that is where a test flips
// Engine mid-stream. Returns the two output channels concatenated block by block.
struct Output { std::vector<float> l, r; };
Output render(SuperConvolverProcessor& proc, pulp::state::StateStore& store,
              const Settings& s, int nblocks, std::size_t host_block = kBlock,
              const std::function<void(pulp::state::StateStore&, int)>& on_block = {}) {
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, kIrSeconds);
    store.set_value(kMix, s.mix);
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, static_cast<float>(s.engine));
    store.set_value(kGpuOnly, static_cast<float>(s.gpu_only));
    proc.prepare(prep_ctx(host_block));

    std::vector<float> in_l(host_block), in_r(host_block);
    std::vector<float> o_l(host_block), o_r(host_block);
    pulp::midi::MidiBuffer mi, mo;
    Output out;
    for (int b = 0; b < nblocks; ++b) {
        if (on_block) on_block(store, b);
        for (std::size_t i = 0; i < host_block; ++i) {
            const std::size_t g = static_cast<std::size_t>(b) * host_block + i;
            in_l[i] = stimulus(g);
            in_r[i] = stimulus(g + 7919);   // decorrelated R
        }
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {o_l.data(), o_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, host_block);
        pulp::audio::BufferView<float> ov(op, 2, host_block);
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = static_cast<int>(host_block);
        proc.process(ov, iv, mi, mo, ctx);
        out.l.insert(out.l.end(), o_l.begin(), o_l.end());
        out.r.insert(out.r.end(), o_r.begin(), o_r.end());
    }
    return out;
}

double peak(const std::vector<float>& v) {
    double p = 0.0;
    for (float x : v) p = std::max(p, static_cast<double>(std::abs(x)));
    return p;
}

/// The tolerance for "the GPU seam is invisible".
///
/// Every comparison in this file is between streams produced by DIFFERENT
/// PartitionedConvolver instances — either two separate SuperConvolverProcessors
/// (one Engine=CPU, one Engine=GPU), or the plugin's convolver against the
/// GpuWorkerStub's. There is no comparison anywhere between two runs of one
/// convolver, so bit-for-bit equality is never the right assertion.
///
/// It is NOT exact equality, and it cannot be. The convolver's FFT is SIMD, and
/// the reduction order of a SIMD accumulation depends on the buffer's ALIGNMENT,
/// which depends on where the allocator happened to put it. Two independent
/// instances convolving the same signal with the same IR therefore agree to
/// within ~1 ULP, not bit-for-bit — and WHICH samples differ changes from process
/// to process, because ASLR moves the heap. MEASURED: `operator==` here fails
/// about half the runs, always on a 1-ULP pair like
/// -0.066829853f vs -0.066829845f (~8e-9).
///
/// 1e-6 keeps every ounce of the test's discriminating power. The property under
/// test is that the CPU net's delay equals the GPU path's delay, and a
/// one-block misalignment moves samples by ~1e-3 — MEASURED, by mutating
/// gpu_extra_ by one block: the differences that appear are ~8.5e-4, a thousand
/// times this tolerance. A seam this test is blind to would be a seam no
/// arithmetic could produce.
constexpr float kSeamTol = 1e-6f;

/// Sample-wise "no seam", with a non-vacuity guard: a comparison of two silent
/// buffers would pass any tolerance.
void require_no_seam(const Output& gpu, const Output& cpu) {
    REQUIRE(gpu.l.size() == cpu.l.size());
    REQUIRE(gpu.r.size() == cpu.r.size());
    REQUIRE(peak(cpu.l) > 0.01);   // there is real signal to disagree about
    REQUIRE(peak(cpu.r) > 0.01);

    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.l.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(gpu.l[i] - cpu.l[i])));
        worst = std::max(worst, static_cast<double>(std::abs(gpu.r[i] - cpu.r[i])));
    }
    INFO("worst |gpu - cpu| = " << worst << " over " << cpu.l.size() << " samples");
    REQUIRE(worst <= kSeamTol);
}

/// Placement check for a MarkerWorker run (Mix = 100 %, GPU only, so a missed block
/// is silence and a hit is the marker verbatim). The plugin's output FIFO starts
/// primed with one internal block of silence, so output block j carries what the
/// (j-1)-th pulp_gpu_xfer call returned — which must be either silence (a miss) or
/// the marker of INPUT block j-1-L. Any other marker value means the wet stream has
/// SHIFTED against the timeline, which is exactly what a dropped or expired block
/// used to do, permanently and silently.
/// Returns the number of hit blocks, so a caller can assert non-vacuity.
int check_marker_alignment(const Output& out) {
    const std::size_t nblocks = out.l.size() / kBlock;
    int hits = 0;
    for (std::size_t j = 1; j < nblocks; ++j) {
        const float v = out.l[j * kBlock];
        for (std::size_t i = 0; i < kBlock; ++i) {
            REQUIRE(out.l[j * kBlock + i] == v);   // a marker block is constant
            REQUIRE(out.r[j * kBlock + i] == v);
        }
        if (v == 0.0f) continue;                   // a miss: silent, because GPU only
        const std::int64_t seq =
            static_cast<std::int64_t>(j) - 1 - static_cast<std::int64_t>(kLat);
        INFO("output block " << j << " carries marker " << v << ", expected block " << seq);
        REQUIRE(seq >= 0);
        REQUIRE(v == marker(seq));
        ++hits;
    }
    return hits;
}

}  // namespace

// The engine's only route to a GPU. Native builds (this one) link it; the wasm
// module imports it from the page's GPU worker (wclap-processor.js's _gpuXfer,
// whose body this mirrors — including the shape guard: BOTH dimensions are
// checked, because the ring writes channels*frames floats back into the plugin's
// buffer and only `channels` can overrun it).
extern "C" int pulp_gpu_xfer(const float* in_planar, float* out_planar, int frames,
                             int channels) {
    const std::uint64_t call = g_seam.calls++;
    if (g_seam.ring == nullptr) return 0;
    if (frames != static_cast<int>(kBlock) || channels != static_cast<int>(kChan)) {
        ++g_seam.rejected;
        return 0;
    }
    if (g_seam.pump) g_seam.pump(call);   // the worker ran during the last block period
    g_seam.ring->push(in_planar);
    const int hit = g_seam.ring->pop(out_planar) ? 1 : 0;
    if (hit) ++g_seam.hits;
    return hit;
}

TEST_CASE("web GPU module declares Engine and GPU only, both defaulting to off",
          "[super-convolver][web-gpu]") {
    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    proc.define_parameters(store);

    REQUIRE(store.param_count() == 6);
    REQUIRE(store.info(kEngine) != nullptr);
    REQUIRE(store.info(kGpuOnly) != nullptr);
    // CPU is the default engine and the safety net is on: the GPU is opt-in.
    REQUIRE(store.get_default(kEngine) == 0.0f);
    REQUIRE(store.get_default(kGpuOnly) == 0.0f);
    // Rooms/Flow are NOT on the web GPU lane (see define_parameters).
    REQUIRE(store.info(kRooms) == nullptr);
    REQUIRE(store.info(kFlow) == nullptr);
}

TEST_CASE("reported latency is the reblock plus the GPU round-trip, for BOTH engines",
          "[super-convolver][web-gpu]") {
    g_seam.reset();
    const int expect = static_cast<int>(kBlock + kLat * kBlock);   // 512 + 1024

    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, kIrSeconds);
    store.set_value(kEngine, 0.0f);
    proc.prepare(prep_ctx(kBlock));

    REQUIRE(proc.latency_samples() == expect);

    // Flipping Engine live must NOT move the reported latency — that would jump
    // the host's PDC mid-stream. It is fixed at prepare for BOTH engines.
    store.set_value(kEngine, 1.0f);
    REQUIRE(proc.latency_samples() == expect);
    REQUIRE(proc.gpu_engine_active());
    store.set_value(kEngine, 0.0f);
    REQUIRE(proc.latency_samples() == expect);
    REQUIRE_FALSE(proc.gpu_engine_active());
}

// THE ALIGNMENT PROOF. With the GPU missing every single block and the safety net
// on, the GPU engine must produce EXACTLY what the CPU engine produces — not
// "close", not "within a tolerance": the same samples. That is only true if the
// CPU wet is delayed by precisely the L blocks the GPU round-trip costs.
TEST_CASE("a missed GPU block is sample-for-sample the CPU engine's output",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 24;

    g_seam.reset();
    SuperConvolverProcessor cpu_proc;
    pulp::state::StateStore cpu_store;
    const Output cpu = render(cpu_proc, cpu_store, {.engine = 0}, kBlocks);

    RingModel ring;                      // a worker that never runs: every block misses
    g_seam.reset();
    g_seam.ring = &ring;
    SuperConvolverProcessor gpu_proc;
    pulp::state::StateStore gpu_store;
    const Output gpu = render(gpu_proc, gpu_store, {.engine = 1, .gpu_only = 0}, kBlocks);

    REQUIRE(g_seam.calls > 0);       // the engine really did try the GPU
    REQUIRE(g_seam.hits == 0);
    REQUIRE(ring.missed > 0);
    require_no_seam(gpu, cpu);       // includes the non-vacuity guard
}

// A delivered GPU block must land exactly where the CPU wet it replaces would
// have: the wet handed back on call j is the wet of INPUT block j-L, and the
// plugin's output FIFO starts primed with one internal block of silence, so it
// occupies output samples [B + jB, B + (j+1)B).
TEST_CASE("a delivered GPU block lands at the block the CPU wet would have",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 12;

    RingModel ring;
    MarkerWorker worker(ring);
    g_seam.reset();
    g_seam.ring = &ring;
    g_seam.pump = [&](std::uint64_t) { worker.run(); };   // a worker that keeps up

    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    // Fully wet at unity gain and no CPU net, so the output IS the GPU wet stream.
    const Output out = render(proc, store, {.mix = 100.0f, .engine = 1, .gpu_only = 1},
                              kBlocks);

    REQUIRE(check_marker_alignment(out) == kBlocks - 1 - static_cast<int>(kLat));

    // The first L blocks have no wet yet (nothing was pushed L blocks ago), so they
    // MISS — that is where the fixed latency comes from, not from primed silence.
    const auto [blocks, misses] = proc.web_gpu_stats();
    REQUIRE(misses == kLat);
    REQUIRE(blocks == g_seam.calls - kLat);
}

// "GPU only" takes the net away: a missed block is SILENT, not CPU-covered. This
// is the mechanism the browser proof depends on to show the GPU is genuinely
// carrying the audio — if it were wrong, the proof would be meaningless.
TEST_CASE("GPU only leaves a missed block silent instead of covering it",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 16;

    RingModel silent_ring;
    g_seam.reset();
    g_seam.ring = &silent_ring;          // no worker: every block misses
    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    // Fully wet: any non-zero sample can only have come from a wet block.
    const Output out = render(proc, store, {.mix = 100.0f, .engine = 1, .gpu_only = 1},
                              kBlocks);
    REQUIRE(peak(out.l) == 0.0);
    REQUIRE(peak(out.r) == 0.0);

    // Not vacuous: the same run WITH the net is loud.
    RingModel net_ring;
    g_seam.reset();
    g_seam.ring = &net_ring;
    SuperConvolverProcessor net_proc;
    pulp::state::StateStore net_store;
    const Output net = render(net_proc, net_store,
                              {.mix = 100.0f, .engine = 1, .gpu_only = 0}, kBlocks);
    REQUIRE(peak(net.l) > 0.01);
}

// The seam itself: with a worker that convolves the same IR and publishes the wet
// of every block it takes (what a real one does), a run that alternates hits and
// misses must STILL equal the all-CPU output exactly — no discontinuity where the
// engine hands over, in either direction.
TEST_CASE("alternating GPU hits and misses reproduce the CPU output exactly",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 32;

    g_seam.reset();
    SuperConvolverProcessor cpu_proc;
    pulp::state::StateStore cpu_store;
    const Output cpu = render(cpu_proc, cpu_store, {.engine = 0}, kBlocks);

    RingModel ring;
    GpuWorkerStub worker(ring, base_ir_for(kIrSeconds));
    g_seam.reset();
    g_seam.ring = &ring;
    // The worker CONSUMES every block (its convolution history depends on it) but
    // only PUBLISHES on every other pass, so it is chronically late and the worklet
    // misses about half the time.
    g_seam.pump = [&](std::uint64_t call) { worker.run(8, call % 2 == 0); };

    SuperConvolverProcessor gpu_proc;
    pulp::state::StateStore gpu_store;
    const Output gpu = render(gpu_proc, gpu_store, {.engine = 1, .gpu_only = 0}, kBlocks);

    const auto [blocks, misses] = gpu_proc.web_gpu_stats();
    REQUIRE(blocks > 0);     // both paths were genuinely exercised
    REQUIRE(misses > 0);
    require_no_seam(gpu, cpu);
}

// A LIVE ENGINE FLIP, in both directions, mid-stream. The whole run must still be
// the CPU engine's output sample-for-sample: on GPU the delivered wet IS the CPU
// wet (same IR, same delay), and on CPU it is the CPU wet.
//
// This is the case a stub that answers in lockstep with the plugin CANNOT model.
// Under a positional ring, the out-ring keeps whatever it holds while Engine=CPU
// (nothing pops it), and the first pops after the flip back to GPU return that
// stale content as if it were current — a burst of pre-flip audio. Here the wets
// carry their block index, so those stale blocks are recognized as stale, dropped,
// and the flip costs at most a couple of CPU-covered misses.
TEST_CASE("flipping Engine live, in both directions, is inaudible",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 40;

    g_seam.reset();
    SuperConvolverProcessor cpu_proc;
    pulp::state::StateStore cpu_store;
    const Output cpu = render(cpu_proc, cpu_store, {.engine = 0}, kBlocks);

    RingModel ring;
    GpuWorkerStub worker(ring, base_ir_for(kIrSeconds));
    g_seam.reset();
    g_seam.ring = &ring;
    g_seam.pump = [&](std::uint64_t) { worker.run(); };   // a worker that keeps up

    SuperConvolverProcessor gpu_proc;
    pulp::state::StateStore gpu_store;
    // GPU for 10 blocks, CPU for 10, GPU again for 10, CPU for the rest.
    const Output gpu = render(
        gpu_proc, gpu_store, {.engine = 1, .gpu_only = 0}, kBlocks, kBlock,
        [](pulp::state::StateStore& store, int b) {
            const int engine = (b < 10 || (b >= 20 && b < 30)) ? 1 : 0;
            store.set_value(kEngine, static_cast<float>(engine));
        });

    // The GPU really did carry audio while it was selected.
    const auto [blocks, misses] = gpu_proc.web_gpu_stats();
    REQUIRE(blocks > 10);
    // And the ring stayed in lockstep across both flips: it dropped a stale wet or
    // two on the way back to GPU rather than emitting them.
    REQUIRE(misses <= 2 * kLat + 2);
    require_no_seam(gpu, cpu);
}

// A WORKER STALL long enough to FILL the input ring, so the audio thread starts
// DROPPING blocks — the throttled-background-tab case, which the design calls a
// normal outcome rather than an exceptional one.
//
// A dropped input means one wet is never produced. Under a positional ring the
// worklet keeps popping one slot per block anyway, so from then on it emits a wet
// that LEADS the dry path by one block per drop — permanently, silently, with
// nothing to correct it. The seq stamps make it cost exactly one CPU-covered MISS.
//
// Asserted with markers rather than a convolution, deliberately: a dropped input
// leaves a real convolver's overlap-save tail genuinely wrong for one IR length
// (physics, not a bug), which would mask the alignment property under test. Every
// delivered block must carry the marker of the block that is exactly L old — never
// a neighbour's.
TEST_CASE("a stalled worker that forces dropped inputs stays aligned",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 40;

    RingModel ring;
    MarkerWorker worker(ring);
    g_seam.reset();
    g_seam.ring = &ring;
    // Stalled for blocks 8..19 (the input ring holds 4, so blocks 12.. are dropped),
    // then it catches up.
    g_seam.pump = [&](std::uint64_t call) {
        if (call >= 8 && call < 20) return;
        worker.run();
    };

    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    const Output out = render(proc, store, {.mix = 100.0f, .engine = 1, .gpu_only = 1},
                              kBlocks);

    REQUIRE(ring.dropped > 0);     // the stall really did overflow the input ring
    REQUIRE(ring.missed > 0);      // …and those blocks came out as misses
    REQUIRE(ring.resynced > 0);    // …and the late wets the stall queued were dropped
    // Every hit landed on the right block, before AND after the stall, and there are
    // plenty of them: the GPU kept carrying audio once it caught up.
    REQUIRE(check_marker_alignment(out) > 15);
}

// An EXPIRED block: the worker consumes the input, misses its deadline, and never
// publishes that wet. The wet stream then has a HOLE. Under a positional ring the
// hole shifts every later wet one block EARLY — silently, with no miss to observe.
// Here pop() sees the next wet's block index run past the one it wants, misses
// that single block, and carries on aligned.
TEST_CASE("a block the worker expires costs one miss, not a permanent shift",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 32;

    g_seam.reset();
    SuperConvolverProcessor cpu_proc;
    pulp::state::StateStore cpu_store;
    const Output cpu = render(cpu_proc, cpu_store, {.engine = 0}, kBlocks);

    RingModel ring;
    GpuWorkerStub worker(ring, base_ir_for(kIrSeconds));
    worker.skip_seq = 12;          // block 12's deadline expires: convolved, never published
    g_seam.reset();
    g_seam.ring = &ring;
    g_seam.pump = [&](std::uint64_t) { worker.run(); };

    SuperConvolverProcessor gpu_proc;
    pulp::state::StateStore gpu_store;
    const Output gpu = render(gpu_proc, gpu_store, {.engine = 1, .gpu_only = 0}, kBlocks);

    REQUIRE(worker.expired == 1);
    REQUIRE(ring.dropped == 0);    // nothing was dropped — this is purely an expiry
    const auto [blocks, misses] = gpu_proc.web_gpu_stats();
    REQUIRE(blocks > 20);
    REQUIRE(misses == kLat + 1);   // the L start-up blocks, plus the expired one
    require_no_seam(gpu, cpu);
}

// The host's shape guard, mirrored natively: the ring writes channels*frames floats
// into the plugin's staging buffer, so a channel-count mismatch must REFUSE the
// lane (0 = miss, CPU covers) rather than overrun it from the audio thread.
TEST_CASE("pulp_gpu_xfer refuses a block whose shape is not the ring's",
          "[super-convolver][web-gpu]") {
    RingModel ring;
    g_seam.reset();
    g_seam.ring = &ring;

    std::vector<float> in(kBlockFloats, 1.0f), out(kBlockFloats, 0.0f);
    REQUIRE(pulp_gpu_xfer(in.data(), out.data(), static_cast<int>(kBlock), 4) == 0);
    REQUIRE(pulp_gpu_xfer(in.data(), out.data(), 128, static_cast<int>(kChan)) == 0);
    REQUIRE(g_seam.rejected == 2);
    REQUIRE(ring.dropped == 0);    // a refused call never even touches the ring
    REQUIRE(ring.missed == 0);
}

// A host block that is not a multiple of the internal block exercises the reblock
// FIFO under the GPU engine (the browser's render quantum is 128, never 512).
TEST_CASE("the GPU engine reblocks a 128-frame host quantum like the CPU engine",
          "[super-convolver][web-gpu]") {
    constexpr int kBlocks = 96;   // 96 * 128 = 24 internal blocks

    g_seam.reset();
    SuperConvolverProcessor cpu_proc;
    pulp::state::StateStore cpu_store;
    const Output cpu = render(cpu_proc, cpu_store, {.engine = 0}, kBlocks, 128);

    RingModel ring;
    GpuWorkerStub worker(ring, base_ir_for(kIrSeconds));
    g_seam.reset();
    g_seam.ring = &ring;
    g_seam.pump = [&](std::uint64_t) { worker.run(); };

    SuperConvolverProcessor gpu_proc;
    pulp::state::StateStore gpu_store;
    const Output gpu = render(gpu_proc, gpu_store, {.engine = 1}, kBlocks, 128);

    REQUIRE(g_seam.calls == 24);
    require_no_seam(gpu, cpu);
}

#else   // the shipped CPU-only web module

// The other half of the contract. A WAM host fetches getParameterInfo() once when
// the module is mounted, so the CPU-only module must never grow the GPU controls:
// they belong to the separate PULP_WEB_GPU_AUDIO artifact. If this count moves,
// the shipped browser demo's control grid has silently changed.
TEST_CASE("the CPU-only web module declares exactly four parameters",
          "[super-convolver][web-gpu]") {
    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    proc.define_parameters(store);

    REQUIRE(store.param_count() == 4);
    REQUIRE(store.info(kMix) != nullptr);
    REQUIRE(store.info(kSize) != nullptr);
    REQUIRE(store.info(kGain) != nullptr);
    REQUIRE(store.info(kBypass) != nullptr);
    REQUIRE(store.info(kEngine) == nullptr);
    REQUIRE(store.info(kGpuOnly) == nullptr);
    REQUIRE_FALSE(proc.gpu_engine_active());
}

TEST_CASE("the CPU-only web module reports only the reblock latency",
          "[super-convolver][web-gpu]") {
    SuperConvolverProcessor proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, kIrSeconds);
    proc.prepare(prep_ctx(kBlock));
    REQUIRE(proc.latency_samples() == static_cast<int>(kBlock));
}

#endif  // PULP_WEB_GPU_AUDIO
