// Bake-layer parameter injection — the production version of Forge Probe A.
//
// Proves that a CONTROL-THREAD parameter write reaches a BAKED custom node and
// changes its audio output sample-accurately, RT-safely, WITHOUT re-baking and
// without touching the live graph's parameter-ingress path. Unlike the Probe A
// spike (a bare shared atomic), this drives the real production path:
//
//   * BakedGraphProcessor::claim_param_injection() + ParamInjector::inject(),
//   * pulp::state::ParameterEvent / ParameterEventQueue reused verbatim,
//   * a per-node single-writer mailbox drained at process(),
//   * ParamCursor sample-accurate interpolation with ramps
//     (sample_offset + ramp_duration_sample_frames), and
//   * per-node exclusive-claim discipline.
//
// Two custom node types exercise the primitive:
//   * DelayNode wraps the real pulp::signal::DelayLine block (the "ONE real
//     block" requirement): its time_ms/feedback params are turned live and the
//     baked node's echo tail changes.
//   * ParamProbeNode outputs the injected param value per sample, so the exact
//     sample-accurate value and ramp interpolation can be asserted.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

// ── ParamProbeNode ──────────────────────────────────────────────────────
// Declares one param (id 1) and writes its current value into every output
// sample: out[k] = params.value_at(1, k). Lets a test assert the exact
// sample-accurate (and ramped) value the injection path delivered.
constexpr pulp::state::ParamID kProbeId = 1;

CustomNodeType make_probe_node() {
    CustomNodeType t;
    t.type_id = "forge_param_probe";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.lowerable = true;
    t.create = []() -> void* { return new int(0); };  // trivial non-null instance
    t.destroy = [](void* p) { delete static_cast<int*>(p); };
    t.baked_params.push_back({kProbeId, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* /*inst*/, pulp::audio::BufferView<float>& out,
           const pulp::audio::BufferView<const float>& /*in*/, int n,
           const BakedParamView& params) {
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                o[static_cast<std::size_t>(k)] =
                    params.value_at(kProbeId, static_cast<std::int32_t>(k));
            }
        };
    return t;
}

// ── DelayNode ───────────────────────────────────────────────────────────
// Wraps pulp::signal::DelayLine as a lowerable custom node with two injectable
// params: time_ms (id 1) and feedback (id 2). A feedback delay so both knobs
// audibly change the output tail.
constexpr pulp::state::ParamID kTimeMsId = 1;
constexpr pulp::state::ParamID kFeedbackId = 2;

struct DelayInstance {
    pulp::signal::DelayLine dl;
    double sample_rate = kSr;
    int max_delay_samples = 0;
};

CustomNodeType make_delay_node() {
    CustomNodeType t;
    t.type_id = "forge_delay";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.lowerable = true;
    t.create = []() -> void* { return new DelayInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DelayInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<DelayInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples = static_cast<int>(sr);  // up to 1s of delay
        s->dl.prepare(s->max_delay_samples);
    };
    // time_ms: 1..500 ms (default 10); feedback: 0..0.95 (default 0.0).
    t.baked_params.push_back({kTimeMsId, 1.0f, 500.0f, 10.0f});
    t.baked_params.push_back({kFeedbackId, 0.0f, 0.95f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, pulp::audio::BufferView<float>& out,
           const pulp::audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<DelayInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float time_ms = params.value_at(kTimeMsId, off);
                const float feedback = params.value_at(kFeedbackId, off);
                float delay_samples =
                    time_ms * static_cast<float>(s->sample_rate) / 1000.0f;
                if (delay_samples < 1.0f) delay_samples = 1.0f;
                if (delay_samples > static_cast<float>(s->max_delay_samples)) {
                    delay_samples = static_cast<float>(s->max_delay_samples);
                }
                const float delayed = s->dl.read(delay_samples);
                const float x = i[static_cast<std::size_t>(k)] + feedback * delayed;
                s->dl.push(x);
                o[static_cast<std::size_t>(k)] = delayed;  // wet (echo) output
            }
        };
    return t;
}

// Build in -> custom -> out, bake it, and prepare the baked processor.
struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;

    explicit BakedFixture(const CustomNodeType& type) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(1, "In");
        custom_node = g.add_custom_node(type.type_id, 1, "Node");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, custom_node, 0));
        REQUIRE(g.connect(custom_node, 0, out, 0));
        g.set_canonical_executor_routing_enabled(true);
        REQUIRE(g.prepare(kSr, kFrames));

        result = bake(g);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = 1;
        pc.output_channels = 1;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

std::vector<float> impulse(int n) {
    std::vector<float> v(static_cast<std::size_t>(n), 0.0f);
    if (n > 0) v[0] = 1.0f;
    return v;
}

// Render one block of `in` through `proc`, returning the output.
std::vector<float> run_block(pulp::format::Processor& proc, const std::vector<float>& in) {
    std::vector<float> input = in;
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in_view(&in_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return output;
}

float tail_energy(const std::vector<float>& block) {
    float e = 0.0f;
    for (std::size_t k = 1; k < block.size(); ++k) e += std::fabs(block[k]);
    return e;
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value,
                                      std::int32_t offset = 0) {
    return {id, offset, value, /*ramp_duration_sample_frames=*/0};
}

} // namespace

TEST_CASE("Baked param injection: control-thread write changes a baked delay's tail",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_delay_node());

    // Claim exclusive injection for the delay node.
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Knob at feedback=0: a single echo, then silence (no recirculation).
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.0f)) == InjectStatus::Ok);
    const float dry_tail = tail_energy(run_block(*fx.result.processor, impulse(kFrames)));

    // Turn feedback up from the control thread → the SAME baked processor must
    // now recirculate, so the tail energy grows. No re-bake occurred.
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.85f)) == InjectStatus::Ok);
    // Feed several silent blocks; the recirculating echo keeps producing output.
    float wet_tail = 0.0f;
    for (int b = 0; b < 8; ++b) {
        wet_tail += tail_energy(run_block(*fx.result.processor,
                                          std::vector<float>(kFrames, 0.0f)));
    }

    INFO("dry_tail=" << dry_tail << " wet_tail=" << wet_tail);
    CHECK(wet_tail > dry_tail);
    CHECK(wet_tail > 0.5f);
}

TEST_CASE("Baked param injection: immediate value is sample-accurate and held across blocks",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Default is 0.0 until the first injection.
    auto b0 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    CHECK(b0.front() == Catch::Approx(0.0f));
    CHECK(b0.back() == Catch::Approx(0.0f));

    // Immediate value at offset 0 → the whole block reads the new value.
    REQUIRE(inj.inject(immediate(kProbeId, 0.6f)) == InjectStatus::Ok);
    auto b1 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b1) CHECK(v == Catch::Approx(0.6f));

    // No new injection → the value is HELD (not reverted to the default).
    auto b2 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b2) CHECK(v == Catch::Approx(0.6f));
}

TEST_CASE("Baked param injection: a mid-block immediate event applies from its offset",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Event scheduled at sample 64: samples [0,64) hold the prior value (0.0),
    // samples [64, n) read the new value.
    constexpr std::int32_t kAt = 64;
    REQUIRE(inj.inject(immediate(kProbeId, 0.9f, kAt)) == InjectStatus::Ok);
    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (int k = 0; k < kAt; ++k) CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(0.0f));
    for (int k = kAt; k < kFrames; ++k) CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(0.9f));
}

TEST_CASE("Baked param injection: a ramp interpolates smoothly, sample-by-sample",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Ramp from 0.0 to 1.0 over R samples starting at offset 0.
    constexpr std::int32_t kRamp = 96;
    pulp::state::ParameterEvent e{kProbeId, /*offset=*/0, /*value=*/1.0f,
                                  /*ramp_duration_sample_frames=*/kRamp};
    REQUIRE(inj.inject(e) == InjectStatus::Ok);

    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));

    // Expect linear interpolation start + (target-start)*k/R for k<R, then held.
    for (int k = 0; k < kFrames; ++k) {
        const float expected =
            k >= kRamp ? 1.0f : static_cast<float>(k) / static_cast<float>(kRamp);
        CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(expected).margin(1e-4));
    }
    // Smoothness: strictly non-decreasing, no zipper step > one sample's slope.
    const float step = 1.0f / static_cast<float>(kRamp);
    for (int k = 1; k < kRamp; ++k) {
        const float d = out[static_cast<std::size_t>(k)] - out[static_cast<std::size_t>(k - 1)];
        CHECK(d == Catch::Approx(step).margin(1e-4));
    }
}

// ── Footgun regression: inject(single) must ACCUMULATE, not replace. Pre-fix,
// each single inject published a fresh one-event snapshot, so two single
// injects to the SAME node with no intervening process() collapsed (latest-
// snapshot-wins) to only the LAST one — a multi-param node silently dropped a
// param. Post-fix they merge into the pending batch and BOTH land.
TEST_CASE("Baked param injection: two single injects to different params both land",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_delay_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Between blocks: time_ms=50 THEN feedback=0.8, as two SINGLE injects.
    // Pre-fix only feedback survived — time_ms silently stayed at its 10 ms
    // default, putting the first echo at 480 samples instead of 2400.
    REQUIRE(inj.inject(immediate(kTimeMsId, 50.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.8f)) == InjectStatus::Ok);

    // Render past the SECOND echo (2 * 2400 = 4800 samples) and concatenate.
    std::vector<float> all;
    for (int b = 0; b < 40; ++b) {  // 40 * 128 = 5120 > 4800
        auto out = run_block(*fx.result.processor,
                             b == 0 ? impulse(kFrames)
                                    : std::vector<float>(kFrames, 0.0f));
        all.insert(all.end(), out.begin(), out.end());
    }
    // time_ms landed: the first echo sits at ~50 ms (DelayLine reads d+1 pushes
    // back, so sample 2401), and NOTHING sits near the 10 ms default position.
    for (int k = 470; k <= 490; ++k) CHECK(std::fabs(all[static_cast<std::size_t>(k)]) < 1e-6f);
    const auto peak_in = [&](int lo, int hi) {
        int idx = lo;
        for (int k = lo; k < hi; ++k) {
            if (std::fabs(all[static_cast<std::size_t>(k)]) >
                std::fabs(all[static_cast<std::size_t>(idx)])) idx = k;
        }
        return idx;
    };
    const int e1 = peak_in(1000, 3600);
    CHECK(e1 >= 2400);
    CHECK(e1 <= 2402);
    CHECK(all[static_cast<std::size_t>(e1)] == Catch::Approx(1.0f).margin(1e-4));
    // feedback landed: the echo recirculates — a second echo one delay later at
    // 0.8x the first.
    const int e2 = peak_in(3600, 5120);
    CHECK(e2 == 2 * e1);
    CHECK(all[static_cast<std::size_t>(e2)] == Catch::Approx(0.8f).margin(1e-3));
}

TEST_CASE("Baked param injection: a single inject into a full unconsumed batch is refused loudly",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_delay_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Publish a FULL-capacity queue of time_ms events, none for feedback, and
    // do not process — the pending batch has no room for another param.
    pulp::state::ParameterEventQueue q;
    for (std::size_t i = 0; i < pulp::state::ParameterEventQueue::kCapacity; ++i) {
        REQUIRE(q.push(immediate(kTimeMsId, 20.0f)));
    }
    REQUIRE(inj.inject(q) == InjectStatus::Ok);

    // The single inject cannot fit; it must be REFUSED (PartialOverflow), not
    // silently evict someone else's pending event.
    CHECK(inj.inject(immediate(kFeedbackId, 0.5f)) == InjectStatus::PartialOverflow);

    // Once a block consumes the pending batch, the same single inject fits.
    run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    CHECK(inj.inject(immediate(kFeedbackId, 0.5f)) == InjectStatus::Ok);
}

TEST_CASE("Baked param injection: latest published queue wins",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Two publishes before a single process() → the latest wins.
    REQUIRE(inj.inject(immediate(kProbeId, 0.2f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(kProbeId, 0.7f)) == InjectStatus::Ok);
    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : out) CHECK(v == Catch::Approx(0.7f));
}

TEST_CASE("Baked param injection: a ParameterEventQueue delivers sample-accurate events",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // A whole queue: two immediate steps at different offsets in one block.
    pulp::state::ParameterEventQueue q;
    REQUIRE(q.push(immediate(kProbeId, 0.3f, 0)));
    REQUIRE(q.push(immediate(kProbeId, 0.8f, 90)));
    REQUIRE(inj.inject(q) == InjectStatus::Ok);

    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (int k = 0; k < 90; ++k) CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(0.3f));
    for (int k = 90; k < kFrames; ++k) CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(0.8f));
}

TEST_CASE("Baked param injection: inject + process() are allocation-free on the audio path",
          "[host][baked][param-injection][forge][rt]") {
    BakedFixture fx(make_delay_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Pre-allocate ALL buffers/views OUTSIDE the probe so only the injection +
    // render path is measured (run_block would allocate its own vectors).
    std::vector<float> input(kFrames, 0.0f);
    std::vector<float> output(kFrames, 0.0f);
    input[0] = 1.0f;  // impulse
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in_view(&in_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;

    // Warm up so any first-call lazy paths are primed before probing.
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.5f)) == InjectStatus::Ok);
    fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);

    pulp::test::RtAllocationProbe probe;
    // A block that BOTH drains a fresh injection AND renders must not allocate;
    // nor must a subsequent block that only holds the value.
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.6f)) == InjectStatus::Ok);
    fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);
    fx.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);
    CHECK(probe.allocation_count() == 0);
    CHECK(probe.allocated_bytes() == 0);
}

TEST_CASE("Baked param injection: per-node exclusive claim is enforced",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());

    ParamInjector first = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(first.valid());

    // A second claim on the same node while the first is live must fail.
    ParamInjector second = fx.baked().claim_param_injection(fx.custom_node);
    CHECK_FALSE(second.valid());

    // Releasing the first makes the node claimable again.
    first = ParamInjector{};  // move-assign an empty handle → releases the claim
    ParamInjector third = fx.baked().claim_param_injection(fx.custom_node);
    CHECK(third.valid());

    // A node with no declared params is never claimable.
    CHECK_FALSE(fx.baked().claim_param_injection(/*bogus node id*/ 99999).valid());
}

TEST_CASE("Baked param injection: repeated concurrent injection stays clean",
          "[host][baked][param-injection][forge][rt]") {
    BakedFixture fx(make_delay_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> injected{0};

    // Control thread hammers injections; audio thread renders continuously. The
    // single-writer mailbox + latest-wins publication must never tear or crash,
    // and the final held value must reflect a real injected value.
    std::thread control([&] {
        float v = 0.0f;
        while (!stop.load(std::memory_order_relaxed)) {
            v = v >= 0.9f ? 0.0f : v + 0.05f;
            inj.inject(immediate(kFeedbackId, v));
            injected.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Render enough blocks that many injections land between them.
    for (int b = 0; b < 4000; ++b) {
        run_block(*fx.result.processor,
                  b == 0 ? impulse(kFrames) : std::vector<float>(kFrames, 0.0f));
    }
    stop.store(true, std::memory_order_relaxed);
    control.join();

    REQUIRE(injected.load() > 0);
    // Settle: inject a known feedback and render — output must stay finite.
    REQUIRE(inj.inject(immediate(kFeedbackId, 0.4f)) == InjectStatus::Ok);
    auto out = run_block(*fx.result.processor, impulse(kFrames));
    for (float v : out) CHECK(std::isfinite(v));
}

// ── C1 regression: a ramp longer than one block must NOT freeze at the block
// boundary. The pre-fix code committed only a SCALAR held value at end of block,
// so a rebuilt per-block cursor lost the in-flight ramp and the glide stalled.
TEST_CASE("Baked param injection: a ramp spanning multiple blocks completes sample-accurately",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Ramp 0.0 -> 1.0 over 960 samples = 7.5 blocks of 128. A single-block-only
    // implementation freezes near 128/960 after block 1 and never reaches 1.0.
    constexpr std::int32_t kRamp = 960;
    pulp::state::ParameterEvent e{kProbeId, /*offset=*/0, /*value=*/1.0f,
                                  /*ramp_duration_sample_frames=*/kRamp};
    REQUIRE(inj.inject(e) == InjectStatus::Ok);

    // Every sample across every block must equal the EXACT global linear value,
    // including across each block boundary (continuity is the whole point).
    constexpr int kBlocks = 10;  // > ceil(960/128) so we also see the held tail
    float last = -1.0f;
    for (int b = 0; b < kBlocks; ++b) {
        auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
        for (int k = 0; k < kFrames; ++k) {
            const int global = b * kFrames + k;
            const double expected =
                global >= kRamp ? 1.0
                                : static_cast<double>(global) / static_cast<double>(kRamp);
            const float v = out[static_cast<std::size_t>(k)];
            CHECK(v == Catch::Approx(expected).margin(1e-4));
            CHECK(v >= last - 1e-4f);  // strictly non-decreasing (no freeze/dip)
            last = v;
        }
    }
    // The last observed value reached the target — proving no mid-ramp freeze.
    CHECK(last == Catch::Approx(1.0f));

    // With no further events the completed ramp HOLDS at the target.
    auto held = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : held) CHECK(v == Catch::Approx(1.0f));
}

TEST_CASE("Baked param injection: a mid-block event during a carried ramp supersedes it",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Start a long ramp 0 -> 1 over 960 samples, let it run one full block so a
    // ramp is in flight at the boundary (carried into block 2).
    REQUIRE(inj.inject(pulp::state::ParameterEvent{kProbeId, 0, 1.0f, 960}) ==
            InjectStatus::Ok);
    run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));  // block 1

    // Block 2: inject an immediate step at offset 64. Samples [0,64) must keep
    // gliding along the CARRIED ramp; [64,n) jump to the new immediate value.
    REQUIRE(inj.inject(immediate(kProbeId, 0.2f, 64)) == InjectStatus::Ok);
    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (int k = 0; k < 64; ++k) {
        const double glided = static_cast<double>(128 + k) / 960.0;  // carried ramp
        CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(glided).margin(1e-4));
    }
    for (int k = 64; k < kFrames; ++k) {
        CHECK(out[static_cast<std::size_t>(k)] == Catch::Approx(0.2f));
    }
}

TEST_CASE("Baked param injection: an overflowed source queue publishes its retained prefix",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    // Fill the source queue to capacity with a known value, then push one more —
    // the extra event is dropped and the queue reports overflow.
    pulp::state::ParameterEventQueue q;
    for (std::size_t i = 0; i < pulp::state::ParameterEventQueue::kCapacity; ++i) {
        REQUIRE(q.push(immediate(kProbeId, 0.6f)));
    }
    REQUIRE_FALSE(q.push(immediate(kProbeId, 0.99f)));  // the dropped event
    REQUIRE(q.overflowed());

    // inject() reports PartialOverflow (distinct from InvalidHandle) AND still
    // publishes the retained prefix, so the node reads a retained value (0.6),
    // never the dropped one (0.99).
    CHECK(inj.inject(q) == InjectStatus::PartialOverflow);
    auto out = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : out) CHECK(v == Catch::Approx(0.6f));

    // Contrast: a dead handle is a DIFFERENT status, not conflated with overflow.
    ParamInjector dead;
    CHECK_FALSE(dead.valid());
    CHECK(dead.inject(q) == InjectStatus::InvalidHandle);
    CHECK(dead.inject(immediate(kProbeId, 0.5f)) == InjectStatus::InvalidHandle);
}

TEST_CASE("Baked param injection: two nodes of the same type inject independently",
          "[host][baked][param-injection][forge]") {
    // Two probe nodes of the SAME type, both feeding the single output port (the
    // executor sums fan-in). If the per-node mailboxes were shared, injecting the
    // same param id into each would collapse to one value; independent per-node
    // namespacing means the output is the SUM of two distinct injected values.
    SignalGraph g;
    const auto type = make_probe_node();
    REQUIRE(g.register_custom_node_type(type));
    const auto in = g.add_input_node(1, "In");
    const auto n1 = g.add_custom_node(type.type_id, 1, "P1");
    const auto n2 = g.add_custom_node(type.type_id, 1, "P2");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, n1, 0));
    REQUIRE(g.connect(in, 0, n2, 0));
    REQUIRE(g.connect(n1, 0, out, 0));
    REQUIRE(g.connect(n2, 0, out, 0));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    LowerResult result = bake(g);
    REQUIRE(result.accepted);
    REQUIRE(result.processor);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr;
    pc.max_buffer_size = kFrames;
    pc.input_channels = 1;
    pc.output_channels = 1;
    result.processor->prepare(pc);
    auto& baked = *static_cast<BakedGraphProcessor*>(result.processor.get());

    ParamInjector i1 = baked.claim_param_injection(n1);
    ParamInjector i2 = baked.claim_param_injection(n2);
    REQUIRE(i1.valid());
    REQUIRE(i2.valid());

    REQUIRE(i1.inject(immediate(kProbeId, 0.3f)) == InjectStatus::Ok);
    REQUIRE(i2.inject(immediate(kProbeId, 0.4f)) == InjectStatus::Ok);
    auto b1 = run_block(*result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b1) CHECK(v == Catch::Approx(0.7f));  // 0.3 + 0.4, isolated

    // Change only node 2; node 1's held value must be unaffected.
    REQUIRE(i2.inject(immediate(kProbeId, 0.1f)) == InjectStatus::Ok);
    auto b2 = run_block(*result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b2) CHECK(v == Catch::Approx(0.4f));  // 0.3 (held) + 0.1
}

TEST_CASE("Baked param injection: an injected value survives a re-prepare",
          "[host][baked][param-injection][forge]") {
    BakedFixture fx(make_probe_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.valid());

    REQUIRE(inj.inject(immediate(kProbeId, 0.6f)) == InjectStatus::Ok);
    auto b1 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b1) CHECK(v == Catch::Approx(0.6f));

    // Re-prepare the SAME processor. The mailbox (and its last published batch)
    // persist across prepare(), and the existing claim stays live — so a block
    // with NO new injection re-applies the last value rather than reverting.
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr;
    pc.max_buffer_size = kFrames;
    pc.input_channels = 1;
    pc.output_channels = 1;
    fx.result.processor->prepare(pc);
    REQUIRE(inj.valid());

    auto b2 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b2) CHECK(v == Catch::Approx(0.6f));

    // The same handle still injects after the re-prepare.
    REQUIRE(inj.inject(immediate(kProbeId, 0.25f)) == InjectStatus::Ok);
    auto b3 = run_block(*fx.result.processor, std::vector<float>(kFrames, 0.0f));
    for (float v : b3) CHECK(v == Catch::Approx(0.25f));
}

TEST_CASE("Baked param injection: a baked-only node does not run its DSP on the live graph",
          "[host][baked][param-injection][forge]") {
    // make_probe_node() provides ONLY process_instance_baked_param (no plain
    // process/process_instance). On the LIVE routed graph that baked-param DSP is
    // NOT consulted: with no live callback the executor falls through to input
    // passthrough. So the live output equals the input — it is transparent, NOT
    // the per-sample param value the baked path would emit (which for the default
    // param would be 0.0). This pins the "baked channel is baked-only" contract.
    SignalGraph g;
    const auto type = make_probe_node();
    REQUIRE(g.register_custom_node_type(type));
    const auto in = g.add_input_node(1, "In");
    const auto node = g.add_custom_node(type.type_id, 1, "Probe");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, node, 0));
    REQUIRE(g.connect(node, 0, out, 0));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    std::vector<float> input = impulse(kFrames);   // non-silent
    std::vector<float> output(static_cast<std::size_t>(kFrames), -1.0f);  // sentinel
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in_view(&in_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
    g.process(out_view, in_view, kFrames);

    // Passthrough of the impulse — the baked DSP (which would emit the param
    // value, 0.0 by default) did not run live.
    for (int k = 0; k < kFrames; ++k) {
        CHECK(output[static_cast<std::size_t>(k)] ==
              Catch::Approx(input[static_cast<std::size_t>(k)]));
    }
}
