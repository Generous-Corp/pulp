// Step LFO — the pattern, the window it plays through, the shift register that can
// replace it, and the two properties a position-derived sequencer must have: it
// lands on the same step however the playhead arrived, and its randomness repeats
// on a re-render.
//
// Scope note: `Processor::process()` and the pure step functions. Nothing here
// proves a step becomes a voltage at a jack.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "step_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;

/// A value no correct output ever emits, pre-loaded into the output buffer.
constexpr float kSentinel = -7.0f;

/// The mode almost every test runs in: locked to the transport, holding when it
/// stops. It is the one beat mode that is a pure function of the position, which
/// is what most of these assertions are about.
constexpr float kLocked = static_cast<float>(static_cast<int>(SyncMode::transport2));

[[nodiscard]] constexpr float as_param(SyncMode m) noexcept {
    return static_cast<float>(static_cast<int>(m));
}
[[nodiscard]] constexpr float as_param(InputRole r) noexcept {
    return static_cast<float>(static_cast<int>(r));
}
[[nodiscard]] constexpr float as_param(InputMode m) noexcept {
    return static_cast<float>(static_cast<int>(m));
}
[[nodiscard]] constexpr float as_param(Range r) noexcept {
    return static_cast<float>(static_cast<int>(r));
}
[[nodiscard]] constexpr float as_param(LengthMode m) noexcept {
    return static_cast<float>(static_cast<int>(m));
}

/// A prepared host. `HeadlessHost` is neither copyable nor movable — it hands the
/// processor a pointer to its own StateStore — so the rig holds one in place.
struct Rig {
    Rig() { host.prepare(kSampleRate, 4096, 2, 2); }

    state::StateStore& state() { return host.state(); }
    const StepProcessor& proc() const {
        return *static_cast<const StepProcessor*>(host.processor());
    }
    void set(state::ParamID id, float v) { host.state().set_value(id, v); }

    /// One step per beat, locked to the transport. The setting most of these tests
    /// want, spelled once.
    void one_step_per_beat() {
        set(StepProcessor::kSyncMode, kLocked);
        set(StepProcessor::kSpeedMode, 1.0f);
        set(StepProcessor::kBeats, 1.0f);
    }

    /// Render one block, returning the CV channel. `input` fills both input
    /// channels sample by sample, so a test can drive the reset and trigger jacks
    /// at an exact offset.
    std::vector<float> render(double position_beats, int frames = 512,
                              bool playing = true, bool bypassed = false,
                              const std::function<void(int, float*, float*)>& input = {}) {
        audio::Buffer<float> in(2, static_cast<std::size_t>(frames));
        audio::Buffer<float> out(2, static_cast<std::size_t>(frames));
        in.clear();
        // A sentinel, not silence. A host hands the plug-in whatever was in the
        // buffer last; clearing it here would hide an output channel the processor
        // forgot to write, and "forgot to write the gate" reads as "the gate is low".
        for (std::size_t c = 0; c < 2; ++c)
            for (int n = 0; n < frames; ++n) out.channel(c)[static_cast<std::size_t>(n)] = kSentinel;
        if (input)
            for (int n = 0; n < frames; ++n)
                input(n, &in.channel(0)[static_cast<std::size_t>(n)],
                      &in.channel(1)[static_cast<std::size_t>(n)]);

        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, static_cast<std::size_t>(frames));
        auto ov = out.view();

        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = frames;
        ctx.is_playing = playing;
        ctx.is_bypassed = bypassed;
        ctx.tempo_bpm = kTempo;
        ctx.position_beats = position_beats;
        ctx.position_samples =
            static_cast<std::int64_t>(position_beats * 60.0 / kTempo * kSampleRate);
        host.process(ov, iv, ctx);

        gate.assign(out.channel(1).begin(), out.channel(1).end());
        return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
    }

    /// Sample one CV value in the middle of the beat, where no glide or gate can
    /// have touched it.
    [[nodiscard]] float at_beat(double beat) { return render(beat, 1)[0]; }

    /// The eight levels, distinct and easily recognised: -1.00, -0.75, ... +0.75.
    void set_ramp() {
        for (int i = 0; i < kMaxSequencerSteps; ++i)
            set(StepProcessor::step_param(i), -1.0f + 0.25f * static_cast<float>(i));
    }
    [[nodiscard]] static float ramp_level(int step) {
        return -1.0f + 0.25f * static_cast<float>(step);
    }

    format::HeadlessHost host{create_step};
    std::vector<float> gate;
};

/// A trigger on the first sample of a block, and nothing after it.
void trigger_right(int n, float*, float* right) { *right = n == 0 ? 1.0f : 0.0f; }
void trigger_left(int n, float* left, float*) { *left = n == 0 ? 1.0f : 0.0f; }

}  // namespace

// ── The pure step functions ──────────────────────────────────────────────────

TEST_CASE("Step LFO descriptor emits a pattern and a gate", "[brew][step]") {
    auto proc = create_step();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "Step LFO");
    REQUIRE(desc.output_buses.size() == 1);
    REQUIRE(desc.output_buses[0].default_channels == 2);
    REQUIRE(desc.input_buses.size() == 1);
    REQUIRE(desc.input_buses[0].default_channels == 2);
}

TEST_CASE("wrap_index is Euclidean, so a step before the origin is still a step",
          "[brew][step]") {
    CHECK(wrap_index(0, 8) == 0);
    CHECK(wrap_index(7, 8) == 7);
    CHECK(wrap_index(8, 8) == 0);
    CHECK(wrap_index(-1, 8) == 7);
    CHECK(wrap_index(-9, 8) == 7);
    CHECK(wrap_index(5, 0) == 0);
}

TEST_CASE("step_fraction never reaches 1.0", "[brew][step]") {
    for (double p = 0.0; p < 4.0; p += 0.013) CHECK(step_fraction(p) < 1.0);
    // The subtraction can round up for a position just below a boundary.
    CHECK(step_fraction(std::nextafter(3.0, 0.0)) < 1.0);
    CHECK(step_fraction(-0.25) > 0.7);
}

TEST_CASE("speed mode forks an LFO from a sequencer", "[brew][step][speed]") {
    // Cycle: one cycle spans the whole window, so the window's length sets the
    // step rate and the pattern's period never changes.
    CHECK(step_position(1.0, SpeedMode::cycle, 4) == 4.0);
    CHECK(step_position(1.0, SpeedMode::cycle, 8) == 8.0);
    // Step: one cycle is one step, so the window's length sets the period.
    CHECK(step_position(1.0, SpeedMode::step, 4) == 1.0);
    CHECK(step_position(1.0, SpeedMode::step, 8) == 1.0);
    CHECK(speed_mode_from_param(0.0f) == SpeedMode::cycle);
    CHECK(speed_mode_from_param(1.0f) == SpeedMode::step);
}

TEST_CASE("the window can be named by its length or by its end", "[brew][step]") {
    CHECK(window_length(LengthMode::start_length, 0, 5, 7) == 5);
    CHECK(window_length(LengthMode::start_end, 0, 5, 3) == 4);
    CHECK(window_length(LengthMode::start_end, 2, 5, 2) == 1);
    // An end before the start wraps rather than yielding an empty pattern.
    CHECK(window_length(LengthMode::start_end, 6, 5, 1) == 4);
    // Out of range clamps, never wraps into a different window.
    CHECK(window_length(LengthMode::start_length, 0, 99, 0) == kMaxSequencerSteps);
    CHECK(window_length(LengthMode::start_length, 0, -3, 0) == 1);
}

TEST_CASE("pattern_index walks the window and wraps across the eight",
          "[brew][step]") {
    CHECK(pattern_index(0, 0, 8) == 0);
    CHECK(pattern_index(9, 0, 8) == 1);
    CHECK(pattern_index(0, 6, 3) == 6);
    CHECK(pattern_index(1, 6, 3) == 7);
    CHECK(pattern_index(2, 6, 3) == 0);
    CHECK(pattern_index(3, 6, 3) == 6);
    CHECK(pattern_index(-1, 0, 8) == 7);
}

TEST_CASE("glide_toward rests on the programmed level for the rest of the step",
          "[brew][step]") {
    CHECK(glide_toward(-1.0f, 1.0f, 0.0, 0.0) == 1.0f);
    CHECK(glide_toward(-1.0f, 1.0f, 0.0, 0.5) == -1.0f);
    CHECK_THAT(glide_toward(-1.0f, 1.0f, 0.25, 0.5), WithinAbs(0.0f, 1e-6f));
    CHECK(glide_toward(-1.0f, 1.0f, 0.5, 0.5) == 1.0f);
    CHECK(glide_toward(-1.0f, 1.0f, 0.9, 0.5) == 1.0f);
}

TEST_CASE("Interpolation Linear is exactly a full-step glide", "[brew][step]") {
    CHECK(effective_glide(Interpolation::stepped, 0.3) == 0.3);
    CHECK(effective_glide(Interpolation::linear, 0.0) == 1.0);
    CHECK(effective_glide(Interpolation::linear, 0.3) == 1.0);
    // A glide outside [0,1] would run the blend past its endpoints.
    CHECK(effective_glide(Interpolation::stepped, 4.0) == 1.0);
    CHECK(effective_glide(Interpolation::stepped, -1.0) == 0.0);
}

TEST_CASE("the gate punches the same hole in the CV that it does in the gate",
          "[brew][step]") {
    CHECK(step_gate_open(0.0, 1.0));
    CHECK(step_gate_open(0.99, 1.0));
    CHECK(step_gate_open(0.4, 0.5));
    CHECK_FALSE(step_gate_open(0.5, 0.5));
    CHECK(step_gated(0.7f, 0.4, 0.5) == 0.7f);
    CHECK(step_gated(0.7f, 0.6, 0.5) == 0.0f);
}

TEST_CASE("Random is zero-preserving and bounded", "[brew][step]") {
    CHECK(step_value(0.3f, 12, 0.0f, 7) == 0.3f);
    for (std::int64_t k = -50; k < 50; ++k) {
        const float v = step_value(0.9f, k, 1.0f, 3);
        CHECK(v >= -1.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("apply_range maps a bipolar step into a unipolar jack", "[brew][step]") {
    CHECK(apply_range(-1.0f, Range::bipolar) == -1.0f);
    CHECK(apply_range(-1.0f, Range::unipolar) == 0.0f);
    CHECK(apply_range(0.0f, Range::unipolar) == 0.5f);
    CHECK(apply_range(1.0f, Range::unipolar) == 1.0f);
}

// ── The shift register ───────────────────────────────────────────────────────

TEST_CASE("flip_probability spans never / half / always", "[brew][step][register]") {
    CHECK(flip_probability(1.0) == 0.0);
    CHECK(flip_probability(0.0) == 0.5);
    CHECK(flip_probability(-1.0) == 1.0);
    CHECK(flip_probability(4.0) == 0.0);
    CHECK(flip_probability(-4.0) == 1.0);
}

TEST_CASE("Randomness +1 locks a loop of Length steps", "[brew][step][register]") {
    const RegisterSettings s{.length = 5, .randomness = 1.0, .seed = 9};
    ShiftRegister r;
    const std::uint64_t origin = r.at(0, s);
    CHECK(r.at(5, s) == origin);
    CHECK(r.at(10, s) == origin);

    // ...and the DAC therefore repeats with period 5, which is what a user hears.
    const DacSettings d = DacSettings::binary();
    for (int k = 0; k < 5; ++k)
        CHECK(dac_value(r.at(k, s), s.length, d) ==
              dac_value(r.at(k + 5, s), s.length, d));
}

TEST_CASE("Randomness -1 locks a loop of twice Length, alternately inverted",
          "[brew][step][register]") {
    const RegisterSettings s{.length = 6, .randomness = -1.0, .seed = 41};
    ShiftRegister r;
    const std::uint64_t origin = r.at(0, s);
    const std::uint64_t mask = register_mask(s.length);
    CHECK(r.at(6, s) == (~origin & mask));
    CHECK(r.at(12, s) == origin);
    // Half a period apart every bit differs. That is the whole of "alternately
    // inverted", and it is the only thing separating -1 from +1 at the DAC.
    CHECK((r.at(3, s) ^ r.at(9, s)) == mask);
}

TEST_CASE("Randomness 0 does not repeat within a Length-step loop",
          "[brew][step][register]") {
    const RegisterSettings s{.length = 8, .randomness = 0.0, .seed = 5};
    ShiftRegister r;
    std::set<std::uint64_t> seen;
    for (int k = 0; k < 64; ++k) seen.insert(r.at(k, s));
    // A locked eight-step loop would visit at most eight states.
    CHECK(seen.size() > 20);
}

TEST_CASE("the register never starts from all zeros", "[brew][step][register]") {
    for (std::uint32_t seed = 0; seed < 256; ++seed)
        for (int len : {1, 2, 8, 48}) CHECK(initial_register(seed, len) != 0ULL);
}

TEST_CASE("a one-bit register still clocks", "[brew][step][register]") {
    // With nowhere to shift, the bit that leaves is the bit that arrives. At
    // Randomness +1 it never inverts, so the single bit is stable.
    const RegisterSettings hold{.length = 1, .randomness = 1.0, .seed = 3};
    ShiftRegister r;
    for (int k = 0; k < 8; ++k) CHECK(r.at(k, hold) == 1ULL);

    // At -1 it inverts every step: the classic divide-by-two.
    const RegisterSettings flip{.length = 1, .randomness = -1.0, .seed = 3};
    ShiftRegister f;
    for (int k = 0; k < 8; ++k)
        CHECK(f.at(k, flip) == static_cast<std::uint64_t>(k % 2 == 0 ? 1 : 0));
}

TEST_CASE("Set Next fills the register with ones", "[brew][step][register]") {
    const RegisterSettings s{
        .length = 8, .randomness = 0.0, .seed = 11, .set_next = true};
    ShiftRegister r;
    CHECK(r.at(8, s) == register_mask(8));
    CHECK(dac_value(r.at(8, s), 8, DacSettings::binary()) == 1.0f);
}

TEST_CASE("the register replays rather than drifting", "[brew][step][register]") {
    const RegisterSettings s{.length = 12, .randomness = 0.0, .seed = 77};
    ShiftRegister forward, backward;
    std::vector<std::uint64_t> want;
    for (int k = 0; k < 40; ++k) want.push_back(forward.at(k, s));

    // A backwards locate: ask for step 39, then 3, then 39 again.
    CHECK(backward.at(39, s) == want[39]);
    CHECK(backward.at(3, s) == want[3]);
    CHECK(backward.at(39, s) == want[39]);

    // Steps before the origin clamp to the origin rather than running the coin
    // toss backwards through an inverse it does not have.
    CHECK(backward.at(-5, s) == want[0]);
}

TEST_CASE("the register's one-step history serves a glide without replaying",
          "[brew][step][register]") {
    const RegisterSettings s{.length = 9, .randomness = 0.0, .seed = 4};
    ShiftRegister truth, r;
    std::vector<std::uint64_t> want;
    for (int k = 0; k < 20; ++k) want.push_back(truth.at(k, s));
    // The alternation a glide produces, sample after sample: k-1, k, k-1, k...
    for (int k = 1; k < 20; ++k) {
        CHECK(r.at(k - 1, s) == want[static_cast<std::size_t>(k - 1)]);
        CHECK(r.at(k, s) == want[static_cast<std::size_t>(k)]);
        CHECK(r.at(k - 1, s) == want[static_cast<std::size_t>(k - 1)]);
    }
}

TEST_CASE("changing a register setting rebuilds the pattern from the origin",
          "[brew][step][register]") {
    const RegisterSettings a{.length = 8, .randomness = 0.0, .seed = 1};
    const RegisterSettings b{.length = 8, .randomness = 0.0, .seed = 2};
    ShiftRegister fresh_a, fresh_b;
    const std::uint64_t a10 = fresh_a.at(10, a);
    const std::uint64_t b10 = fresh_b.at(10, b);
    REQUIRE(a10 != b10);   // otherwise the rest of this proves nothing

    // The pattern is a function of the settings, not of the order they arrived in.
    // Switching to `b` at step 10 must produce `b`'s bits at step 10, replayed from
    // the origin — not `a`'s state relabelled.
    ShiftRegister r;
    CHECK(r.at(10, a) == a10);
    CHECK(r.at(10, b) == b10);
    CHECK(r.at(10, a) == a10);
}

TEST_CASE("the DAC's window is a weighted sum, normalized or divided",
          "[brew][step][register]") {
    DacSettings d = DacSettings::binary();
    // All eight bits set, automatic scale: exactly full.
    CHECK(dac_value(0xFFULL, 8, d) == 1.0f);
    CHECK(dac_value(0x00ULL, 8, d) == 0.0f);

    // One bit read: it is a gate.
    d.bits = 1;
    CHECK(dac_value(0x01ULL, 8, d) == 1.0f);
    CHECK(dac_value(0x02ULL, 8, d) == 0.0f);

    // Rotate moves the window, and it wraps.
    d.rotate = 1;
    CHECK(dac_value(0x02ULL, 8, d) == 1.0f);
    d.rotate = -1;
    CHECK(dac_value(0x80ULL, 8, d) == 1.0f);

    // A manual divisor keeps its own headroom; a degenerate one yields zero
    // rather than a sign flip under the user's fingertip.
    d = DacSettings::binary();
    d.automatic_scale = false;
    d.scale = 4.0f;
    // Bit 0 carries weight 1.0 — it is the DAC's most significant.
    CHECK_THAT(dac_value(0x01ULL, 8, d), WithinAbs(0.25f, 1e-6f));
    d.scale = 0.0f;
    CHECK(dac_value(0xFFULL, 8, d) == 0.0f);
    d.scale = -1.0f;
    CHECK(dac_value(0xFFULL, 8, d) == 0.0f);
    // Clamped: a hand-set ladder can sum past its own divisor.
    d.scale = 0.5f;
    CHECK(dac_value(0xFFULL, 8, d) == 1.0f);
}

TEST_CASE("a flattened DAC weight silences its bit", "[brew][step][register]") {
    DacSettings d = DacSettings::binary();
    d.bits = 2;
    d.weights[1] = 0.0f;
    // Only the top bit contributes, so the second one cannot move the output.
    CHECK(dac_value(0x01ULL, 8, d) == 1.0f);
    CHECK(dac_value(0x03ULL, 8, d) == 1.0f);
    CHECK(dac_value(0x02ULL, 8, d) == 0.0f);
}

TEST_CASE("wrap_bit keeps a negative rotate inside the ring",
          "[brew][step][register]") {
    CHECK(wrap_bit(0, 8) == 0);
    CHECK(wrap_bit(-1, 8) == 7);
    CHECK(wrap_bit(9, 8) == 1);
    CHECK(wrap_bit(3, 0) == 0);
}

// ── The processor's controls ─────────────────────────────────────────────────

TEST_CASE("every declared control exists as a parameter", "[brew][step]") {
    Rig r;
    for (const auto& c : StepProcessor::controls()) {
        INFO("control " << c.label);
        CHECK(r.state().info(c.id) != nullptr);
    }
    for (const auto& c : StepProcessor::toggles()) {
        INFO("toggle " << c.label);
        CHECK(r.state().info(c.id) != nullptr);
    }
    for (int i = 0; i < kMaxSequencerSteps; ++i)
        CHECK(r.state().info(StepProcessor::step_param(i)) != nullptr);
    for (int i = 0; i < kMaxDacBits; ++i)
        CHECK(r.state().info(StepProcessor::weight_param(i)) != nullptr);
}

TEST_CASE("the DAC's default ladder is binary, most significant first",
          "[brew][step][register]") {
    Rig r;
    const auto d = r.proc().dac_settings();
    for (int i = 0; i < kMaxDacBits; ++i)
        CHECK_THAT(d.weights[static_cast<std::size_t>(i)],
                   WithinAbs(default_dac_weight(i), 1e-9f));
    CHECK(d.automatic_scale);
    CHECK(d.bits == kMaxDacBits);
}

// ── The position rule ────────────────────────────────────────────────────────

TEST_CASE("the step index is a pure function of position", "[brew][step][safety]") {
    // The whole design. Bar 57 plays the step bar 57 plays, whatever the playhead
    // did to get there — so a bounce is reproducible and a locate lands right.
    Rig played, located;
    for (Rig* r : {&played, &located}) {
        r->set_ramp();
        r->one_step_per_beat();
    }

    constexpr int kFrames = 512;
    const double span = (kTempo / (60.0 * kSampleRate)) * kFrames;

    std::vector<float> from_playing;
    for (int b = 0; b <= 40; ++b) from_playing = played.render(span * b, kFrames);
    const auto from_locate = located.render(span * 40.0, kFrames);

    REQUIRE(from_playing == from_locate);
}

TEST_CASE("the CV holds each step's programmed level", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    for (int i = 0; i < kMaxSequencerSteps; ++i)
        CHECK_THAT(r.at_beat(static_cast<double>(i) + 0.5),
                   WithinAbs(Rig::ramp_level(i), 1e-5f));
}

TEST_CASE("Per Step turns the cycle into one step", "[brew][step][speed]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    // One beat per step: beat 3.5 is the middle of step 3.
    CHECK_THAT(r.at_beat(3.5), WithinAbs(Rig::ramp_level(3), 1e-5f));

    // Off, one beat is the whole eight-step window: beat 3.5 is step 28, i.e. 4.
    r.set(StepProcessor::kSpeedMode, 0.0f);
    CHECK_THAT(r.at_beat(3.5), WithinAbs(Rig::ramp_level(4), 1e-5f));
}

TEST_CASE("in cycle mode the window's length does not change the period",
          "[brew][step][speed]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, kLocked);
    r.set(StepProcessor::kBeats, 4.0f);   // four beats per cycle, whatever the length

    // The last step of the window always lands just before the cycle wraps.
    for (float len : {8.0f, 4.0f, 2.0f}) {
        r.set(StepProcessor::kLength, len);
        const int last = static_cast<int>(len) - 1;
        INFO("length " << len);
        CHECK_THAT(r.at_beat(4.0 - 4.0 / (2.0 * len)), WithinAbs(Rig::ramp_level(last), 1e-5f));
    }
}

TEST_CASE("the window shortens the pattern and Start slides it", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kLength, 3.0f);
    r.set(StepProcessor::kStart, 6.0f);   // one-based

    // Zero-based steps 5, 6, 7, 5, 6, 7 ...
    const int expect[6] = {5, 6, 7, 5, 6, 7};
    for (int b = 0; b < 6; ++b) {
        INFO("beat " << b);
        CHECK_THAT(r.at_beat(static_cast<double>(b) + 0.5),
                   WithinAbs(Rig::ramp_level(expect[b]), 1e-5f));
    }
}

TEST_CASE("steps outside the window keep their levels for when it grows",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kLength, 2.0f);
    CHECK_THAT(r.at_beat(0.5), WithinAbs(Rig::ramp_level(0), 1e-5f));
    CHECK_THAT(r.at_beat(2.5), WithinAbs(Rig::ramp_level(0), 1e-5f));
    r.set(StepProcessor::kLength, 8.0f);
    CHECK_THAT(r.at_beat(2.5), WithinAbs(Rig::ramp_level(2), 1e-5f));
}

TEST_CASE("Bounds = End reads End instead of Length", "[brew][step]") {
    Rig r;
    r.set(StepProcessor::kStart, 3.0f);
    r.set(StepProcessor::kEnd, 5.0f);
    r.set(StepProcessor::kLength, 8.0f);
    CHECK(r.proc().window() == 8);
    r.set(StepProcessor::kLengthMode, as_param(LengthMode::start_end));
    CHECK(r.proc().window() == 3);
    // Wrapping: start 7, end 2 (one-based) covers 7, 8, 1, 2.
    r.set(StepProcessor::kStart, 7.0f);
    r.set(StepProcessor::kEnd, 2.0f);
    CHECK(r.proc().window() == 4);
}

// ── The output stage ─────────────────────────────────────────────────────────

TEST_CASE("the gate falls with the CV, at the same fraction", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kGate, 0.5f);

    (void)r.render(2.25, 1);   // first quarter of step 2
    CHECK(r.gate[0] == 1.0f);
    const auto v = r.render(2.75, 1);   // past the gate
    CHECK(r.gate[0] == 0.0f);
    CHECK(v[0] == 0.0f);
}

TEST_CASE("Gate at 1.0 never falls", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    (void)r.render(0.0, 512);
    for (float g : r.gate) CHECK(g == 1.0f);
}

TEST_CASE("Range Unipolar lifts the pattern above zero", "[brew][step]") {
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), -1.0f);
    r.set(StepProcessor::kSyncMode, kLocked);
    CHECK_THAT(r.at_beat(0.5), WithinAbs(-1.0f, 1e-6f));
    r.set(StepProcessor::kRange, as_param(Range::unipolar));
    CHECK_THAT(r.at_beat(0.5), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Offset shifts the whole pattern, gaps included", "[brew][step]") {
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), 0.0f);
    r.one_step_per_beat();
    r.set(StepProcessor::kGate, 0.5f);
    r.set(StepProcessor::kLevelOffset, 0.4f);
    CHECK_THAT(r.at_beat(0.25), WithinAbs(0.4f, 1e-6f));
    // A gap is a voltage like any other: `Offset` lifts it too.
    CHECK_THAT(r.at_beat(0.75), WithinAbs(0.4f, 1e-6f));
}

TEST_CASE("Invert and Out act on the CV at the jack", "[brew][step]") {
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), 0.8f);
    r.set(StepProcessor::kSyncMode, kLocked);
    r.set(StepProcessor::kOutputScale, 0.5f);
    CHECK_THAT(r.at_beat(0.5), WithinAbs(0.4f, 1e-6f));
    r.set(StepProcessor::kInvert, 1.0f);
    CHECK_THAT(r.at_beat(0.5), WithinAbs(-0.4f, 1e-6f));
}

TEST_CASE("a bypassed generator is silent on both jacks", "[brew][step][safety]") {
    Rig r;
    r.set_ramp();
    const auto v = r.render(1.0, 64, /*playing=*/true, /*bypassed=*/true);
    for (float x : v) CHECK(x == 0.0f);
    for (float g : r.gate) CHECK(g == 0.0f);
    CHECK(r.proc().display_step() == -1);
}

// ── Randomness ───────────────────────────────────────────────────────────────

TEST_CASE("Random repeats on a re-render and is off at zero", "[brew][step]") {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        r->set_ramp();
        r->one_step_per_beat();
        r->set(StepProcessor::kRandom, 0.7f);
        r->set(StepProcessor::kSeed, 42.0f);
    }
    const auto first = a.render(3.0, 256);
    const auto second = b.render(3.0, 256);
    for (std::size_t n = 0; n < first.size(); ++n) CHECK(first[n] == second[n]);

    // At zero the pattern is exactly what the editor shows.
    Rig c;
    c.set_ramp();
    c.one_step_per_beat();
    c.set(StepProcessor::kSeed, 42.0f);
    CHECK_THAT(c.at_beat(3.5), WithinAbs(Rig::ramp_level(3), 1e-5f));
}

TEST_CASE("a different Seed rerolls the dither", "[brew][step]") {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        r->set_ramp();
        r->one_step_per_beat();
        r->set(StepProcessor::kRandom, 0.7f);
    }
    a.set(StepProcessor::kSeed, 1.0f);
    b.set(StepProcessor::kSeed, 2.0f);
    CHECK(a.at_beat(3.5) != b.at_beat(3.5));
}

TEST_CASE("Random does not repeat every window length", "[brew][step]") {
    // Keyed on the absolute step, not the wrapped one: the shape loops, the
    // dither does not.
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), 0.0f);
    r.one_step_per_beat();
    r.set(StepProcessor::kLength, 2.0f);
    r.set(StepProcessor::kRandom, 0.6f);
    r.set(StepProcessor::kSeed, 5.0f);
    CHECK(r.at_beat(0.5) != r.at_beat(2.5));
}

// ── The register, driving the pattern ────────────────────────────────────────

TEST_CASE("the register drives the steps when it is on", "[brew][step][register]") {
    Rig r;
    // A flat pattern: any variation must be the register's.
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), 0.0f);
    r.one_step_per_beat();
    CHECK_THAT(r.at_beat(0.5), WithinAbs(0.0f, 1e-6f));

    r.set(StepProcessor::kRegisterOn, 1.0f);
    r.set(StepProcessor::kRandomness, 0.0f);
    r.set(StepProcessor::kSeed, 13.0f);
    std::set<float> levels;
    for (int b = 0; b < 24; ++b) levels.insert(r.at_beat(static_cast<double>(b) + 0.5));
    CHECK(levels.size() > 4);
    for (float v : levels) {
        CHECK(v >= -1.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("the register's levels agree with the pure transfer",
          "[brew][step][register]") {
    Rig r;
    r.set(StepProcessor::kRegisterOn, 1.0f);
    r.set(StepProcessor::kRandomness, 0.0f);
    r.set(StepProcessor::kSeed, 21.0f);
    const auto rs = r.proc().register_settings();
    const auto d = r.proc().dac_settings();
    ShiftRegister own, mirror;
    for (std::int64_t k = 0; k < 16; ++k) {
        const float want = 2.0f * dac_value(mirror.at(k, rs), rs.length, d) - 1.0f;
        CHECK_THAT(r.proc().level_at(k, own), WithinAbs(want, 1e-6f));
    }
}

TEST_CASE("Register + Randomness +1 repeats with the register's own period",
          "[brew][step][register]") {
    Rig r;
    r.one_step_per_beat();
    r.set(StepProcessor::kRegisterOn, 1.0f);
    r.set(StepProcessor::kRegisterLength, 5.0f);
    r.set(StepProcessor::kRandomness, 1.0f);
    r.set(StepProcessor::kSeed, 33.0f);

    std::vector<float> first, again;
    for (int b = 0; b < 5; ++b) first.push_back(r.at_beat(static_cast<double>(b) + 0.5));
    for (int b = 5; b < 10; ++b) again.push_back(r.at_beat(static_cast<double>(b) + 0.5));
    for (std::size_t i = 0; i < first.size(); ++i)
        CHECK_THAT(first[i], WithinAbs(again[i], 1e-6f));
}

TEST_CASE("Random still dithers the register's levels", "[brew][step][register]") {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        r->one_step_per_beat();
        r->set(StepProcessor::kRegisterOn, 1.0f);
        r->set(StepProcessor::kRandomness, 1.0f);
        r->set(StepProcessor::kSeed, 8.0f);
    }
    b.set(StepProcessor::kRandom, 0.5f);
    // The same register, one dithered. Somewhere in the first eight steps they part.
    bool differ = false;
    for (int s = 0; s < 8 && !differ; ++s)
        differ = a.at_beat(static_cast<double>(s) + 0.5) != b.at_beat(static_cast<double>(s) + 0.5);
    CHECK(differ);
}

TEST_CASE("display_bits is dark unless the register is driving",
          "[brew][step][register]") {
    Rig r;
    r.set(StepProcessor::kSyncMode, kLocked);
    (void)r.render(0.5, 64);
    CHECK(r.proc().display_bits() == 0ULL);
    r.set(StepProcessor::kRegisterOn, 1.0f);
    (void)r.render(0.5, 64);
    CHECK(r.proc().display_bits() != 0ULL);
}

// ── The clock, inherited from the LFO ────────────────────────────────────────

TEST_CASE("the two trigger modes join Free and Tempo as non-deterministic",
          "[brew][step][safety]") {
    CHECK(sync_is_deterministic(SyncMode::transport2));
    CHECK(sync_is_deterministic(SyncMode::free3));
    CHECK_FALSE(sync_is_deterministic(SyncMode::free));
    CHECK_FALSE(sync_is_deterministic(SyncMode::tempo));
    CHECK_FALSE(sync_is_deterministic(SyncMode::trig_free));
    CHECK_FALSE(sync_is_deterministic(SyncMode::trig_tempo));
    CHECK(sync_pauses_for_trigger(SyncMode::trig_free));
    CHECK(sync_pauses_for_trigger(SyncMode::trig_tempo));
    CHECK_FALSE(sync_pauses_for_trigger(SyncMode::free));
    // A trigger mode reads the rate knob its base mode reads.
    CHECK(sync_uses_hertz(SyncMode::trig_free));
    CHECK(sync_uses_beats(SyncMode::trig_tempo));
}

TEST_CASE("the Sync knob reaches every mode the Step LFO has", "[brew][step]") {
    Rig r;
    const auto* info = r.state().info(StepProcessor::kSyncMode);
    REQUIRE(info != nullptr);
    CHECK(info->range.max == static_cast<float>(kStepSyncModeCount - 1));
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::trig_tempo));
    CHECK(r.proc().sync_mode() == SyncMode::trig_tempo);
}

TEST_CASE("Free3 reads the timeline in hertz", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::free3));
    r.set(StepProcessor::kSpeedHz, 1.0f);
    r.set(StepProcessor::kSpeedMode, 1.0f);   // one cycle is one step, one per second

    // 2.5 seconds in is the middle of step 2. At 120 bpm that is beat 5.
    CHECK_THAT(r.at_beat(5.0), WithinAbs(Rig::ramp_level(2), 1e-5f));
}

TEST_CASE("the Multiplier is a decade switch on Speed", "[brew][step]") {
    Rig r;
    CHECK_THAT(static_cast<float>(r.proc().hz()), WithinAbs(1.0f, 1e-6f));
    r.set(StepProcessor::kMultiplier,
          static_cast<float>(static_cast<int>(Multiplier::ten)));
    CHECK_THAT(static_cast<float>(r.proc().hz()), WithinAbs(10.0f, 1e-6f));
    r.set(StepProcessor::kMultiplier,
          static_cast<float>(static_cast<int>(Multiplier::tenth)));
    CHECK_THAT(static_cast<float>(r.proc().hz()), WithinAbs(0.1f, 1e-6f));
}

TEST_CASE("Beats, Divisor and Triplet name the cycle in beats", "[brew][step]") {
    Rig r;
    CHECK_THAT(static_cast<float>(r.proc().cycle_length_beats()), WithinAbs(4.0f, 1e-6f));
    r.set(StepProcessor::kBeats, 3.0f);
    r.set(StepProcessor::kDivisor, static_cast<float>(static_cast<int>(NoteUnit::eighth)));
    CHECK_THAT(static_cast<float>(r.proc().cycle_length_beats()), WithinAbs(1.5f, 1e-6f));
    r.set(StepProcessor::kTriplet, 1.0f);
    CHECK_THAT(static_cast<float>(r.proc().cycle_length_beats()), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Phase slides the pattern along the cycle", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    CHECK_THAT(r.at_beat(0.5), WithinAbs(Rig::ramp_level(0), 1e-5f));
    // Half a cycle is half a step here, so beat 0.5 lands at the start of step 1.
    r.set(StepProcessor::kPhaseDegrees, 180.0f);
    CHECK_THAT(r.at_beat(0.5), WithinAbs(Rig::ramp_level(1), 1e-5f));
}

TEST_CASE("Asymmetry warps the cycle, and 0.5 is a wire", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, kLocked);
    r.set(StepProcessor::kBeats, 8.0f);   // eight beats, eight steps in the window

    const float straight = r.at_beat(1.5);
    r.set(StepProcessor::kAsymmetry, 0.25f);
    // Early steps become shorter, so 1.5 beats is further into the pattern.
    CHECK(r.at_beat(1.5) > straight);

    r.set(StepProcessor::kAsymmetry, 0.5f);
    CHECK_THAT(r.at_beat(1.5), WithinAbs(straight, 1e-6f));
}

TEST_CASE("Swing warps the beat timeline while the transport rolls", "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kBeats, 0.25f);   // four steps to the beat

    // A swung eighth means a sounding beat stands for an earlier straight one, so
    // the pattern has not reached as far.
    const float straight = r.at_beat(0.6);
    CHECK_THAT(straight, WithinAbs(Rig::ramp_level(2), 1e-5f));
    r.set(StepProcessor::kSwingPercent, 70.0f);
    const float swung = r.at_beat(0.6);
    CHECK_THAT(swung, WithinAbs(Rig::ramp_level(1), 1e-5f));

    // Parked, there is nothing to push late.
    CHECK_THAT(r.render(0.6, 1, /*playing=*/false)[0], WithinAbs(straight, 1e-5f));
}

TEST_CASE("Smooth slews the step edges and zero is a wire", "[brew][step]") {
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i)
        r.set(StepProcessor::step_param(i), i % 2 == 0 ? -1.0f : 1.0f);
    r.one_step_per_beat();

    const auto hard = r.render(0.0, 64);
    CHECK(hard[0] == -1.0f);

    r.set(StepProcessor::kSmoothMs, 100.0f);   // slew
    (void)r.render(0.0, 64);
    const auto slewed = r.render(1.0, 64);
    // A 100 ms slew cannot cross a full swing in 64 samples.
    CHECK(slewed[63] < 1.0f);
    CHECK(slewed[63] > slewed[0]);
}

// ── The input bus ────────────────────────────────────────────────────────────

TEST_CASE("the inputs are off by default", "[brew][step][safety]") {
    Rig r;
    CHECK(r.proc().input_role(0) == InputRole::off);
    CHECK(r.proc().input_role(1) == InputRole::off);
    CHECK(r.proc().signal_mode() == InputMode::off);

    // A drum loop at full scale on both channels changes nothing.
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, kLocked);
    const auto quiet = r.render(1.5, 64);
    auto drive_both = [](int, float* l, float* rr) { *l = 1.0f; *rr = 1.0f; };
    const auto loud = r.render(1.5, 64, true, false, drive_both);
    for (std::size_t n = 0; n < quiet.size(); ++n) CHECK(quiet[n] == loud[n]);

    // ...and a Signal Mode with no channel routed to it reads nothing either. The
    // routing is what opens the jack, not the mode.
    r.set(StepProcessor::kSignalMode, as_param(InputMode::add));
    const auto still_quiet = r.render(1.5, 64, true, false, drive_both);
    for (std::size_t n = 0; n < quiet.size(); ++n) CHECK(quiet[n] == still_quiet[n]);
}

TEST_CASE("a Signal input is summed, multiplied, or both", "[brew][step]") {
    Rig r;
    for (int i = 0; i < kMaxSequencerSteps; ++i) r.set(StepProcessor::step_param(i), 0.5f);
    r.set(StepProcessor::kSyncMode, kLocked);
    r.set(StepProcessor::kInputLeft, as_param(InputRole::signal));

    auto drive = [](int, float* l, float*) { *l = 0.25f; };
    r.set(StepProcessor::kSignalMode, as_param(InputMode::add));
    CHECK_THAT(r.render(0.5, 1, true, false, drive)[0], WithinAbs(0.75f, 1e-6f));
    r.set(StepProcessor::kSignalMode, as_param(InputMode::multiply));
    CHECK_THAT(r.render(0.5, 1, true, false, drive)[0], WithinAbs(0.125f, 1e-6f));
    r.set(StepProcessor::kSignalMode, as_param(InputMode::combine));
    CHECK_THAT(r.render(0.5, 1, true, false, drive)[0], WithinAbs(0.875f, 1e-6f));
}

TEST_CASE("the trigger detector needs a fall before it fires again", "[brew][step]") {
    TriggerDetector d;
    CHECK(d.process(kTriggerHigh));
    CHECK_FALSE(d.process(1.0f));
    CHECK_FALSE(d.process(0.3f));   // above kTriggerLow: still latched
    CHECK_FALSE(d.process(0.2f));   // re-armed, but not a rising edge yet
    CHECK(d.process(1.0f));
}

TEST_CASE("the trigger detector rides out noise on a slewed edge", "[brew][step]") {
    // The reason the two thresholds are apart. A modular's gate output slews, and a
    // bare comparator on a slewed edge with any noise on it fires a handful of times
    // on the way up — which, in a trigger mode, skips several steps per gate.
    TriggerDetector d;
    CHECK(d.process(0.6f));
    for (float v : {0.4f, 0.6f, 0.45f, 0.7f, 0.3f, 0.9f}) {
        INFO("wobbling at " << v << " — above kTriggerLow, so still latched");
        CHECK_FALSE(d.process(v));
    }
    // All the way down, and only then does the next edge count.
    CHECK_FALSE(d.process(0.0f));
    CHECK(d.process(1.0f));
}

TEST_CASE("a Reset input restarts the pattern at the window's first step",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kInputLeft, as_param(InputRole::reset));

    // Without a reset, beat 3.5 plays step 3.
    CHECK_THAT(r.at_beat(3.5), WithinAbs(Rig::ramp_level(3), 1e-5f));

    // A reset on the first sample makes beat 3.5 the very start of step 0.
    Rig s;
    s.set_ramp();
    s.one_step_per_beat();
    s.set(StepProcessor::kInputLeft, as_param(InputRole::reset));
    const auto v = s.render(3.5, 4, true, false, trigger_left);
    CHECK_THAT(v[0], WithinAbs(Rig::ramp_level(0), 1e-5f));
}

TEST_CASE("Trig modes hold at the end of a step until a trigger arrives",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::trig_free));
    r.set(StepProcessor::kSpeedMode, 1.0f);
    r.set(StepProcessor::kSpeedHz, 100.0f);   // a step every 480 samples
    r.set(StepProcessor::kInputRight, as_param(InputRole::trigger));

    // Long enough for the clock to run well past the first step; the pattern must
    // still be on step 0.
    const auto held = r.render(0.0, 2048);
    for (float v : held) CHECK_THAT(v, WithinAbs(Rig::ramp_level(0), 1e-5f));
    CHECK(r.proc().display_step() == 0);

    // One trigger releases exactly one step.
    const auto stepped = r.render(0.0, 2048, true, false, trigger_right);
    CHECK_THAT(stepped[0], WithinAbs(Rig::ramp_level(1), 1e-5f));
    CHECK_THAT(stepped[2047], WithinAbs(Rig::ramp_level(1), 1e-5f));
    CHECK(r.proc().display_step() == 1);
}

TEST_CASE("a released step starts at the trigger rather than being skipped",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::trig_free));
    r.set(StepProcessor::kSpeedMode, 1.0f);
    r.set(StepProcessor::kSpeedHz, 100.0f);
    r.set(StepProcessor::kGate, 0.5f);
    r.set(StepProcessor::kInputRight, as_param(InputRole::trigger));

    // Wait out several steps' worth of clock, then trigger. The released step's
    // gate must open at the trigger — not be already closed because the clock ran
    // ahead while the pattern waited.
    (void)r.render(0.0, 4096);
    (void)r.render(0.0, 4, true, false, trigger_right);
    CHECK(r.gate[0] == 1.0f);
    CHECK(r.gate[3] == 1.0f);
}

TEST_CASE("a Reset input restarts the step count a Trig mode is holding",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::trig_free));
    r.set(StepProcessor::kSpeedMode, 1.0f);
    r.set(StepProcessor::kSpeedHz, 100.0f);
    r.set(StepProcessor::kInputLeft, as_param(InputRole::reset));
    r.set(StepProcessor::kInputRight, as_param(InputRole::trigger));

    for (int k = 0; k < 3; ++k) (void)r.render(0.0, 256, true, false, trigger_right);
    REQUIRE(r.proc().display_step() == 3);

    // The reset takes the pattern back to step 0 *and* to waiting for its first
    // trigger. Moving the origin without resetting the count would drop it at step
    // 3 of a pattern that has not started.
    const auto v = r.render(0.0, 2048, true, false, trigger_left);
    CHECK(r.proc().display_step() == 0);
    for (float x : v) CHECK_THAT(x, WithinAbs(Rig::ramp_level(0), 1e-5f));
}

TEST_CASE("Trig modes still advance one step per trigger after many",
          "[brew][step]") {
    Rig r;
    r.set_ramp();
    r.set(StepProcessor::kSyncMode, as_param(SyncMode::trig_tempo));
    r.set(StepProcessor::kSpeedMode, 1.0f);
    r.set(StepProcessor::kBeats, 0.0625f);   // fast enough to always be waiting
    r.set(StepProcessor::kInputRight, as_param(InputRole::trigger));

    for (int k = 1; k <= 5; ++k) {
        (void)r.render(0.0, 256, true, false, trigger_right);
        INFO("after " << k << " triggers");
        CHECK(r.proc().display_step() == k % kMaxSequencerSteps);
    }
}

TEST_CASE("a trigger on a non-trigger mode does not stall the pattern",
          "[brew][step]") {
    // `allowed_steps_` is only read by the modes that pause. A stray trigger on a
    // locked pattern must not freeze it.
    Rig r;
    r.set_ramp();
    r.one_step_per_beat();
    r.set(StepProcessor::kInputRight, as_param(InputRole::trigger));
    (void)r.render(0.0, 4, true, false, trigger_right);
    CHECK_THAT(r.at_beat(3.5), WithinAbs(Rig::ramp_level(3), 1e-5f));
}

TEST_CASE("display_step names the step the window is playing, not the raw index",
          "[brew][step]") {
    Rig r;
    r.one_step_per_beat();
    r.set(StepProcessor::kStart, 5.0f);
    r.set(StepProcessor::kLength, 2.0f);
    (void)r.render(0.5, 1);
    CHECK(r.proc().display_step() == 4);
    (void)r.render(1.5, 1);
    CHECK(r.proc().display_step() == 5);
    (void)r.render(2.5, 1);
    CHECK(r.proc().display_step() == 4);
}

TEST_CASE("a zero-length block is survivable", "[brew][step][safety]") {
    Rig r;
    CHECK(r.render(0.0, 0).empty());
}
