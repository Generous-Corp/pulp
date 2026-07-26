// Tape machine — bake-layer catalog suite.
//
// The DSP's own acceptance suite (test_signal_tape_machine.cpp) proves the
// physics. This one proves the NODE: that all three realizations bake and run,
// that every declared parameter reaches the stage it names over the real
// injection path, that the ranges the registry will mirror match the module's
// canonical constants, and that the process callback is allocation-free.
//
// Measurements are made through the baked graph rather than on the DSP class,
// on purpose — a parameter that is declared but never wired, or wired to the
// wrong setter, is exactly the class of bug a test against the class cannot
// see.
//
// Stereo throughout, via the shared fixture's `BakedNodeFixture<2>`. That is
// not incidental: the crosstalk parameter is the one control here that has no
// meaning at all in mono, and it is the one this suite has to drive with one
// channel silent.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_tape_catalog.hpp>

#include <cmath>
#include <vector>

using namespace pulp::host;
namespace tape_node = pulp::host::tape;
namespace sig_tape = pulp::signal::tape;  // `tape` alone is ambiguous against pulp::host::tape
using Catch::Matchers::WithinAbs;
using pulp::test::immediate;
using pulp::signal::TapeArchetype;
using pulp::signal::TapeCurve;
using pulp::signal::TapeMachine;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 512;
/// 750 Hz is 64 samples at 48 kHz, so a block holds whole cycles and a coherent
/// DFT over one block is leakage-free.
constexpr double kToneHz = 750.0;

using Fixture = pulp::test::BakedNodeFixture<2>;

const std::vector<TapeArchetype> kAllArchetypes = {TapeArchetype::ampex_350_440,
                                                   TapeArchetype::studer_a800,
                                                   TapeArchetype::cassette_deck};

std::vector<float> sine(float amplitude) {
    return pulp::test::sine_block(kFrames, kToneHz, kSr, amplitude);
}
std::vector<float> silence() {
    return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
}

/// Steady state of the node for a repeated stereo input. Generous block count:
/// this module carries a 4× oversampling wrap, a 2 ms modulation line and a
/// long print-through line, so a handful of blocks is not settled.
std::vector<std::vector<float>> settle(Fixture& fixture,
                                       const std::vector<std::vector<float>>& input,
                                       int blocks = 48) {
    return fixture.settle(input, blocks);
}

double rms(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) sum += static_cast<double>(v) * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double fundamental(const std::vector<float>& x) {
    return pulp::test::harmonic_magnitude(x, 1, kToneHz, kSr);
}

/// Ratio of harmonic 2..5 energy to the fundamental — enough to see the drive
/// and bias controls move without needing a long analysis window.
double distortion_ratio(const std::vector<float>& x) {
    double harmonics = 0.0;
    for (int k = 2; k <= 5; ++k) {
        const double v = pulp::test::harmonic_magnitude(x, k, kToneHz, kSr);
        harmonics += v * v;
    }
    return std::sqrt(harmonics) / std::max(fundamental(x), 1e-15);
}

}  // namespace

TEST_CASE("Forge tape: every realization bakes and runs in stereo",
          "[host][baked][forge][forge-tape]") {
    for (const TapeArchetype archetype : kAllArchetypes) {
        Fixture fixture(tape_node::make_tape_machine_node(archetype), kSr, kFrames);
        const auto out = settle(fixture, {sine(0.3f), sine(0.3f)});
        REQUIRE(out.size() == 2u);
        for (const auto& channel : out) {
            REQUIRE(rms(channel) > 0.0);
            for (const float v : channel) REQUIRE(std::isfinite(v));
        }
    }
}

TEST_CASE("Forge tape: the three realizations have distinct stable type ids",
          "[host][baked][forge][forge-tape]") {
    // Stable ids are a serialization contract: a baked artifact names its node
    // types by string, so a rename silently orphans every saved graph.
    REQUIRE(std::string(tape_node::tape_type_id(TapeArchetype::ampex_350_440)) ==
            "tape.ampex350_440");
    REQUIRE(std::string(tape_node::tape_type_id(TapeArchetype::studer_a800)) ==
            "tape.studer_a800");
    REQUIRE(std::string(tape_node::tape_type_id(TapeArchetype::cassette_deck)) ==
            "tape.cassette");

    for (const TapeArchetype archetype : kAllArchetypes) {
        const CustomNodeType type = tape_node::make_tape_machine_node(archetype);
        REQUIRE(type.num_input_ports == 2);
        REQUIRE(type.num_output_ports == 2);
        REQUIRE(type.lowerable);
        REQUIRE_FALSE(type.default_name.empty());
    }
}

TEST_CASE("Forge tape: the baked table mirrors the module's canonical ranges",
          "[host][baked][forge][forge-tape]") {
    // Series contract §6: the catalog's ranges ARE the module's contract in real
    // units, so they are asserted against the module's own constants rather than
    // against copies. A drift here is a Forge registry that lies about the DSP.
    for (const TapeArchetype archetype : kAllArchetypes) {
        const CustomNodeType type = tape_node::make_tape_machine_node(archetype);
        const sig_tape::ArchetypePreset preset = sig_tape::archetype_preset(archetype);

        auto row = [&](pulp::state::ParamID id) {
            for (const auto& p : type.baked_params)
                if (p.id == id) return p;
            FAIL("parameter not declared: " << id);
            return type.baked_params.front();
        };
        auto has_row = [&](pulp::state::ParamID id) {
            return std::any_of(type.baked_params.begin(), type.baked_params.end(),
                               [id](const auto& p) { return p.id == id; });
        };

        const auto bias = row(tape_node::kBias);
        REQUIRE_THAT(bias.min_value, WithinAbs(TapeMachine::kBiasMin, 1e-6));
        REQUIRE_THAT(bias.max_value, WithinAbs(TapeMachine::kBiasMax, 1e-6));
        REQUIRE_THAT(bias.default_value, WithinAbs(TapeMachine::kBiasDefault, 1e-6));

        const auto crosstalk = row(tape_node::kCrosstalkDb);
        REQUIRE_THAT(crosstalk.min_value, WithinAbs(TapeMachine::kCrosstalkDbMin, 1e-6));
        REQUIRE_THAT(crosstalk.max_value, WithinAbs(TapeMachine::kCrosstalkDbMax, 1e-6));
        REQUIRE_THAT(crosstalk.default_value, WithinAbs(preset.crosstalk_db, 1e-4));

        const auto print = row(tape_node::kPrintThroughDb);
        REQUIRE_THAT(print.min_value, WithinAbs(TapeMachine::kPrintThroughDbMin, 1e-6));
        REQUIRE_THAT(print.max_value, WithinAbs(TapeMachine::kPrintThroughDbMax, 1e-6));
        // The declared bounds ARE the age table's extremes, so no baked value
        // can request a level the age axis cannot produce.
        REQUIRE_THAT(print.min_value,
                     WithinAbs(static_cast<float>(sig_tape::age_print_through_db(0.0)), 1e-4));
        REQUIRE_THAT(print.max_value,
                     WithinAbs(static_cast<float>(sig_tape::age_print_through_db(1.0)), 1e-4));
        REQUIRE_THAT(print.default_value,
                     WithinAbs(static_cast<float>(sig_tape::age_print_through_db(preset.age01)),
                               1e-4));

        const auto offset = row(tape_node::kPrintOffsetMs);
        REQUIRE_THAT(offset.min_value,
                     WithinAbs(TapeMachine::kPrintThroughOffsetMsMin, 1e-6));
        REQUIRE_THAT(offset.max_value,
                     WithinAbs(TapeMachine::kPrintThroughOffsetMsMax, 1e-6));

        const auto age = row(tape_node::kAge);
        REQUIRE_THAT(age.default_value, WithinAbs(preset.age01, 1e-6));

        const auto companding = row(tape_node::kCompanding);
        REQUIRE_THAT(companding.default_value, WithinAbs(preset.companding ? 1.0f : 0.0f,
                                                         1e-6));

        // The EQ curve's range is the archetype's front panel, not the enum.
        // A fixed realization omits the row instead of exposing a dead knob.
        const tape_node::CurveRange range = tape_node::curve_range(archetype);
        if (range.min_curve != range.max_curve) {
            const auto curve = row(tape_node::kEqCurve);
            REQUIRE_THAT(curve.min_value,
                         WithinAbs(static_cast<float>(range.min_curve), 1e-6));
            REQUIRE_THAT(curve.max_value,
                         WithinAbs(static_cast<float>(range.max_curve), 1e-6));
            REQUIRE_THAT(curve.default_value,
                         WithinAbs(static_cast<float>(preset.default_curve), 1e-6));
        } else {
            REQUIRE_FALSE(has_row(tape_node::kEqCurve));
        }

        const auto mix = row(tape_node::kMix);
        REQUIRE_THAT(mix.min_value, WithinAbs(0.0f, 1e-6));
        REQUIRE_THAT(mix.max_value, WithinAbs(1.0f, 1e-6));
        REQUIRE_THAT(mix.default_value, WithinAbs(1.0f, 1e-6));
    }

    // An Ampex-class node is NAB-only: its curve range is a single point, so
    // the parameter cannot select a curve the machine never had.
    const tape_node::CurveRange ampex = tape_node::curve_range(TapeArchetype::ampex_350_440);
    REQUIRE(ampex.min_curve == ampex.max_curve);
    REQUIRE(ampex.min_curve == TapeCurve::nab);
    // The A800's published master-EQ switch, and the cassette tape-type switch.
    const tape_node::CurveRange studer = tape_node::curve_range(TapeArchetype::studer_a800);
    REQUIRE(studer.min_curve == TapeCurve::nab);
    REQUIRE(studer.max_curve == TapeCurve::iec_ccir);
    const tape_node::CurveRange cassette =
        tape_node::curve_range(TapeArchetype::cassette_deck);
    REQUIRE(cassette.min_curve == TapeCurve::cassette_type1);
    REQUIRE(cassette.max_curve == TapeCurve::cassette_type2);
}

TEST_CASE("Forge tape: speed is a realization, and it snaps to the legal set",
          "[host][baked][forge][forge-tape]") {
    // Speed is NOT a baked parameter — see the catalog header on why a control
    // that reaches an FFT-based filter redesign cannot be applied on the audio
    // thread. Assert the absence, because "we forgot to declare it" and "we
    // deliberately did not" look identical in a table.
    for (const TapeArchetype archetype : kAllArchetypes) {
        const CustomNodeType type = tape_node::make_tape_machine_node(archetype);
        for (const auto& p : type.baked_params) {
            REQUIRE(p.id != 0);
            REQUIRE(p.min_value <= p.max_value);
            REQUIRE(p.default_value >= p.min_value);
            REQUIRE(p.default_value <= p.max_value);
        }
        const bool variable_curve =
            tape_node::curve_range(archetype).min_curve !=
            tape_node::curve_range(archetype).max_curve;
        REQUIRE(type.baked_params.size() == (variable_curve ? 9u : 8u));
    }

    // A cassette node asked for 30 ips runs at 1.875 rather than refusing, so a
    // caller does not have to know the legal table.
    Fixture cassette(tape_node::make_tape_machine_node(TapeArchetype::cassette_deck, 30.0),
                     kSr, kFrames);
    const auto out = settle(cassette, {sine(0.3f), sine(0.3f)});
    for (const float v : out.front()) REQUIRE(std::isfinite(v));
}

TEST_CASE("Forge tape speed and pre-echo identities are stable and distinct",
          "[host][forge][forge-tape][identity]") {
    using A = TapeArchetype;
    const auto base = tape_node::make_tape_machine_node(A::studer_a800);
    REQUIRE(base.type_id == tape_node::kStuderTypeId);
    REQUIRE(tape_node::make_tape_machine_node(A::studer_a800).type_id == base.type_id);
    REQUIRE(tape_node::make_tape_machine_node(A::studer_a800, 30.0).type_id != base.type_id);
    REQUIRE(tape_node::make_tape_machine_node(A::studer_a800, 29.9).type_id ==
            tape_node::make_tape_machine_node(A::studer_a800, 30.0).type_id);
    REQUIRE(tape_node::make_tape_machine_node(A::studer_a800, 15.0, true).type_id != base.type_id);
    const auto pre_echo = tape_node::make_tape_machine_node(A::studer_a800, 15.0, true);
    REQUIRE(pre_echo.type_id ==
            tape_node::make_tape_machine_node(A::studer_a800, 15.0, true).type_id);
    REQUIRE(std::none_of(pre_echo.baked_params.begin(), pre_echo.baked_params.end(),
                         [](const auto& p) { return p.id == tape_node::kPrintOffsetMs; }));
}

TEST_CASE("Forge tape: injecting drive changes harmonic content",
          "[host][baked][param-injection][forge][forge-tape]") {
    Fixture fixture(tape_node::make_tape_machine_node(TapeArchetype::studer_a800), kSr,
                    kFrames);
    ParamInjector injector = fixture.claim_injector();
    const auto tone = sine(0.2f);

    REQUIRE(injector.inject(immediate(tape_node::kAge, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kBias, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kCompanding, 0.0f)) == InjectStatus::Ok);

    REQUIRE(injector.inject(immediate(tape_node::kDrive, 0.0f)) == InjectStatus::Ok);
    const double clean = distortion_ratio(settle(fixture, {tone, tone}).front());

    REQUIRE(injector.inject(immediate(tape_node::kDrive, 1.0f)) == InjectStatus::Ok);
    const double dirty = distortion_ratio(settle(fixture, {tone, tone}).front());

    REQUIRE(dirty > clean * 1.5);
}

TEST_CASE("Forge tape: injecting crosstalk leaks one channel into the other",
          "[host][baked][param-injection][forge][forge-tape]") {
    // The one parameter that only exists because this node is stereo. Left
    // driven, right silent; the right output is pure leak.
    Fixture fixture(tape_node::make_tape_machine_node(TapeArchetype::studer_a800), kSr,
                    kFrames);
    ParamInjector injector = fixture.claim_injector();

    REQUIRE(injector.inject(immediate(tape_node::kAge, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kDrive, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kCompanding, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kPrintThroughDb, -80.0f)) ==
            InjectStatus::Ok);

    double previous = -1e9;
    for (const float configured : {-45.0f, -35.0f, -25.0f}) {
        REQUIRE(injector.inject(immediate(tape_node::kCrosstalkDb, configured)) ==
                InjectStatus::Ok);
        const auto out = settle(fixture, {sine(0.2f), silence()});
        const double leak_db =
            20.0 * std::log10(fundamental(out[1]) / fundamental(out[0]));
        // The configured figure, measured over the production path. The
        // tolerance is looser than the DSP suite's ±1 dB because the node also
        // carries the archetype's default age and its wow/flutter smearing.
        REQUIRE_THAT(leak_db, WithinAbs(static_cast<double>(configured), 2.0));
        REQUIRE(leak_db > previous);
        previous = leak_db;
    }
}

TEST_CASE("Forge tape: injecting the EQ curve reaches the reproduce network",
          "[host][baked][param-injection][forge][forge-tape]") {
    // Type I against Type II on a cassette node. The record and playback
    // networks are exact reciprocals, so the curve is NOT audible as a gain
    // change on programme — the route it travels is the tape's own noise floor,
    // which the reproduce network shapes. Driving this through silence measures
    // exactly that.
    Fixture fixture(tape_node::make_tape_machine_node(TapeArchetype::cassette_deck), kSr,
                    kFrames);
    ParamInjector injector = fixture.claim_injector();
    REQUIRE(injector.inject(immediate(tape_node::kAge, 0.6f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kCompanding, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kPrintThroughDb, -80.0f)) ==
            InjectStatus::Ok);

    auto floor_for = [&](TapeCurve curve) {
        REQUIRE(injector.inject(immediate(tape_node::kEqCurve,
                                          static_cast<float>(curve))) == InjectStatus::Ok);
        return 20.0 * std::log10(std::max(rms(settle(fixture, {silence(), silence()}, 64)
                                                  .front()),
                                          1e-30));
    };

    const double type1 = floor_for(TapeCurve::cassette_type1);
    const double type2 = floor_for(TapeCurve::cassette_type2);
    // Type II's shorter treble constant means a reproduce network that lifts
    // more of the hiss band. Direction, not magnitude — the magnitude is the
    // DSP suite's job.
    REQUIRE(type2 > type1);
}

TEST_CASE("Forge tape: injecting age moves wear and the print-through level together",
          "[host][baked][param-injection][forge][forge-tape]") {
    // The ordering trap this node has to get right: `set_age` WRITES the
    // print-through level, so a block that applies age after the print
    // parameter would silently discard the parameter. Assert the parameter wins.
    Fixture fixture(tape_node::make_tape_machine_node(TapeArchetype::studer_a800), kSr,
                    kFrames);
    ParamInjector injector = fixture.claim_injector();
    REQUIRE(injector.inject(immediate(tape_node::kCompanding, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kDrive, 0.0f)) == InjectStatus::Ok);

    auto floor_at_age = [&](float age) {
        REQUIRE(injector.inject(immediate(tape_node::kAge, age)) == InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(tape_node::kPrintThroughDb, -80.0f)) ==
                InjectStatus::Ok);
        return 20.0 * std::log10(
                          std::max(rms(settle(fixture, {silence(), silence()}, 64).front()),
                                   1e-30));
    };

    // A worn machine hisses more. Drives the reused age table through the node.
    const double fresh = floor_at_age(0.0f);
    const double worn = floor_at_age(0.8f);
    REQUIRE(worn > fresh + 6.0);

    // ...and the explicit print level still takes effect at a fixed age, which
    // is what proves the ordering. An impulse-free check: raise the print level
    // and the output on a repeating tone gains a delayed copy of itself, so its
    // energy rises.
    REQUIRE(injector.inject(immediate(tape_node::kAge, 0.3f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kPrintOffsetMs, 200.0f)) ==
            InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(tape_node::kPrintThroughDb, -80.0f)) ==
            InjectStatus::Ok);
    const double quiet_print = rms(settle(fixture, {sine(0.3f), sine(0.3f)}, 64).front());
    REQUIRE(injector.inject(immediate(tape_node::kPrintThroughDb, -38.0f)) ==
            InjectStatus::Ok);
    const double loud_print = rms(settle(fixture, {sine(0.3f), sine(0.3f)}, 64).front());
    REQUIRE(loud_print != quiet_print);
}

TEST_CASE("Forge tape: injecting mix crossfades against the dry path",
          "[host][baked][param-injection][forge][forge-tape]") {
    Fixture fixture(tape_node::make_tape_machine_node(TapeArchetype::cassette_deck), kSr,
                    kFrames);
    ParamInjector injector = fixture.claim_injector();
    const auto tone = sine(0.3f);

    REQUIRE(injector.inject(immediate(tape_node::kMix, 0.0f)) == InjectStatus::Ok);
    const auto dry = settle(fixture, {tone, tone});
    for (int k = 0; k < kFrames; ++k)
        REQUIRE_THAT(static_cast<double>(dry[0][static_cast<std::size_t>(k)]),
                     WithinAbs(static_cast<double>(tone[static_cast<std::size_t>(k)]), 1e-5));

    REQUIRE(injector.inject(immediate(tape_node::kMix, 1.0f)) == InjectStatus::Ok);
    const auto wet = settle(fixture, {tone, tone});
    double difference = 0.0;
    for (int k = 0; k < kFrames; ++k)
        difference += std::abs(static_cast<double>(wet[0][static_cast<std::size_t>(k)]) -
                               tone[static_cast<std::size_t>(k)]);
    REQUIRE(difference > 1e-3);
}

TEST_CASE("Forge tape: the registry's insertion bound matches the DSP bound",
          "[host][baked][forge][forge-tape]") {
    // Series law 8, in the shape a feed-forward design takes it. There is no
    // feedback path, so `worst_case_gain` does not apply and the catalog says so
    // by name rather than by omission — that flag is asserted here so a future
    // edit that adds feedback without revisiting the registry fails loudly.
    REQUIRE_FALSE(tape_node::kWorstCaseGainApplicable);

    for (const TapeArchetype archetype : kAllArchetypes) {
        TapeMachine probe;
        probe.set_archetype(archetype);
        probe.prepare(kSr);
        REQUIRE_THAT(
            static_cast<double>(tape_node::tape_machine_insertion_gain_bound(archetype, kSr)),
            WithinAbs(probe.worst_case_insertion_gain(), 1e-3));
        REQUIRE(tape_node::tape_machine_insertion_gain_bound(archetype, kSr) > 1.0f);
    }
}

TEST_CASE("Forge tape: the node's process path allocates nothing",
          "[host][baked][forge][forge-tape][rt-safety]") {
    // Buffers and views are built OUTSIDE the probe via `ReusableRenderer` —
    // the fixture's convenience `render()` constructs its own output vectors,
    // so driving it from inside a probe would report the harness's allocations
    // as the node's.
    //
    // The injected values MOVE on every block, which is the point: this node's
    // setters are applied only when a parameter changed, so a probe that held
    // them still would prove nothing about the expensive paths — `set_age`
    // re-derives the reused loss cascade and recomputes an alignment gain over
    // a 385-tap FIR, and it has to do all of that without touching the heap.
    for (const TapeArchetype archetype : kAllArchetypes) {
        for (const bool pre_echo : {false, true}) {
            Fixture fixture(tape_node::make_tape_machine_node(archetype, 0.0, pre_echo),
                            kSr, kFrames);
            ParamInjector injector = fixture.claim_injector();
            const auto tone = sine(0.3f);
            settle(fixture, {tone, tone});  // warm every lazily-touched path first

            pulp::test::ReusableRenderer<2> renderer(fixture, {tone, tone});

            pulp::test::RtAllocationProbe probe;
            for (int block = 0; block < 16; ++block) {
                const auto f = static_cast<float>(block);
                injector.inject(immediate(tape_node::kBias, 0.1f * f - 0.7f));
                injector.inject(immediate(tape_node::kDrive, 0.05f * f));
                injector.inject(immediate(tape_node::kAge, 0.05f * f));
                injector.inject(immediate(tape_node::kCrosstalkDb, -44.0f + f));
                injector.inject(immediate(tape_node::kPrintThroughDb, -79.0f + 2.0f * f));
                if (!pre_echo)
                    injector.inject(
                        immediate(tape_node::kPrintOffsetMs, 210.0f + 20.0f * f));
                injector.inject(immediate(tape_node::kCompanding, block % 2 ? 1.0f : 0.0f));
                injector.inject(immediate(tape_node::kMix, 0.05f * f));
                renderer.render();
            }
            REQUIRE(probe.allocation_count() == 0);
        }
    }
}
