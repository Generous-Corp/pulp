#pragma once

// Step LFO — an eight-step pattern on the LFO's clock.
//
// Two outputs: the stepped control voltage on channel 0, and a gate on channel 1
// that goes high for the gated part of every step. The gate is what lets a step
// pattern drive an envelope downstream rather than only a level.
//
// The timing spine is the LFO's, literally: the same `PhaseClock`, the same eight
// sync modes, the same Speed × Multiplier / Beats × Divisor × Triplet rate pair,
// the same Phase, Swing, Asymmetry, Offset and Smooth. What a cycle *means* is the
// only difference, and it is a knob: in `cycle` the pattern's window is one cycle,
// in `step` each step is. Two further sync modes appear here and nowhere else —
// `Trig Free` and `Trig Tempo` run the clock at the base rate but hold the pattern
// at the end of every step until a trigger arrives on the input bus.
//
// Where a step's level comes from is the other axis. Off, it is one of the eight
// programmed levels. On, it is a weighted DAC reading a looping shift register
// whose feedback bit is inverted with a probability the `Randomness` knob sets:
// +1 locks a pattern of `Length` steps, -1 locks one of twice that alternately
// inverted, and 0 never repeats. Either way `Random` then adds a bounded dither.
//
// Everything above is a pure function of the elapsed cycle count, so bar 57 always
// plays step 3 and two bounces render the same samples — except in the modes that
// say otherwise (`Free`, `Tempo`, and the two trigger modes; see
// `sync_is_deterministic`) and except for `Smooth`, which by definition carries
// state between blocks.
//
// Two things this does not do, named so nobody assumes otherwise. It cannot roll a
// random pattern *into* the eight step knobs, where it could then be hand-edited
// and stored — the register's randomness stays in the register. And the DAC's
// divisor is either the sum of the weights or a number the user sets; there is no
// third mode that adapts it to what the register has lately been doing.

#include <brew/cv.hpp>
#include <brew/lfo.hpp>
#include <brew/phase_clock.hpp>
#include <brew/shift_register.hpp>
#include <brew/smooth.hpp>
#include <brew/param_text.hpp>
#include <brew/step.hpp>
#include <brew/sync.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Defaults that name an enumerator rather than a magic float. A scoped enum does
/// not convert to float on its own, and spelling the double cast at each use site
/// is how one of them ends up naming a different mode than its comment claims.
inline constexpr float kStepDefaultSync =
    static_cast<float>(static_cast<int>(SyncMode::transport));
inline constexpr float kStepDefaultMultiplier =
    static_cast<float>(static_cast<int>(Multiplier::one));
inline constexpr float kStepDefaultDivisor =
    static_cast<float>(static_cast<int>(NoteUnit::quarter));
inline constexpr float kStepDefaultLengthMode =
    static_cast<float>(static_cast<int>(LengthMode::start_length));
inline constexpr float kStepDefaultRange =
    static_cast<float>(static_cast<int>(Range::bipolar));
inline constexpr float kStepDefaultInterp =
    static_cast<float>(static_cast<int>(Interpolation::stepped));
inline constexpr float kStepDefaultRole =
    static_cast<float>(static_cast<int>(InputRole::off));
inline constexpr float kStepDefaultSignalMode =
    static_cast<float>(static_cast<int>(InputMode::off));

class StepProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kBeats = 1,
        kSpeedMode = 2,
        kLength = 3,
        kGlide = 4,
        kRandom = 5,
        kSeed = 6,
        kOutputScale = 7,
        kInvert = 8,
        kGate = 9,
        /// The eight step levels occupy 16..23, leaving room below for controls.
        kStep1 = 16,
        // The LFO's timing spine, in the LFO's order.
        kSyncMode = 24,
        kSpeedHz = 25,
        kMultiplier = 26,
        kDivisor = 27,
        kTriplet = 28,
        kPhaseDegrees = 29,
        kSwingPercent = 30,
        kSwingUnit = 31,
        kAsymmetry = 32,
        kLevelOffset = 33,
        kSmoothMs = 34,
        // The window into the eight steps.
        kStart = 35,
        kEnd = 36,
        kLengthMode = 37,
        kRange = 38,
        kInterpolation = 39,
        // The shift register.
        kRegisterOn = 40,
        kRegisterLength = 41,
        kDacBits = 42,
        kRandomness = 43,
        kRotate = 44,
        kSetNext = 45,
        kAutoScale = 46,
        kDacScale = 47,
        // The input bus.
        kInputLeft = 48,
        kInputRight = 49,
        kSignalMode = 50,
        /// The eight DAC bit weights occupy 56..63.
        kDacWeight1 = 56,
    };

    [[nodiscard]] static constexpr state::ParamID step_param(int i) noexcept {
        return static_cast<state::ParamID>(kStep1 + i);
    }

    [[nodiscard]] static constexpr state::ParamID weight_param(int i) noexcept {
        return static_cast<state::ParamID>(kDacWeight1 + i);
    }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Step LFO",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.step",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Reset / Trigger / Signal", 2}},
            .output_buses = {{"Steps / Gate", 2}},
        };
    }

    /// One row of a control table: the id and the label the editor shows. Kept
    /// beside the parameter definitions so a control cannot be defined and then
    /// left off the panel.
    struct ControlSpec {
        state::ParamID id;
        const char* label;
    };

    // ── How each control reads, here and in the host ─────────────────────────

    static std::string length_mode_name(float v) {
        static const char* const kNames[] = {"Len", "End"};
        return text::named_at(kNames, kLengthModeCount, v);
    }
    static std::string interpolation_name(float v) {
        static const char* const kNames[] = {"Step", "Lin"};
        return text::named_at(kNames, kInterpolationCount, v);
    }
    static std::string input_role_name(float v) {
        static const char* const kNames[] = {"Off", "Reset", "Trig", "Sig"};
        return text::named_at(kNames, kInputRoleCount, v);
    }
    static std::string range_name(float v) {
        static const char* const kNames[] = {"Bipolar", "Unipolar"};
        return text::named_at(kNames, kRangeCount, v);
    }
    static std::string speed_mode_name(float v) {
        static const char* const kNames[] = {"Cycle", "Step"};
        return text::named_at(kNames, 2, v);
    }

    void define_parameters(state::StateStore& store) override {
        // ── Rate ─────────────────────────────────────────────────────────────
        store.add_parameter({.id = kSyncMode,
                             .name = "Sync",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kStepSyncModeCount - 1),
                                       kStepDefaultSync, 1.0f},
                             .to_string = text::step_sync_name});
        store.add_parameter({.id = kSpeedHz,
                             .name = "Speed",
                             .unit = "Hz",
                             .range = {static_cast<float>(kMinFreeHz), 100.0f, 1.0f,
                                       0.0f},
                             .to_string = text::compact});
        store.add_parameter({.id = kMultiplier,
                             .name = "Multiplier",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kMultiplierCount - 1),
                                       kStepDefaultMultiplier, 1.0f},
                             .to_string = text::multiplier_name});
        store.add_parameter({.id = kBeats,
                             .name = "Beats",
                             .unit = "",
                             .range = {0.0625f, 64.0f, 4.0f, 0.0f},
                             .to_string = text::compact});
        store.add_parameter({.id = kDivisor,
                             .name = "Divisor",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kNoteUnitCount - 1),
                                       kStepDefaultDivisor, 1.0f},
                             .to_string = text::divisor_name});
        store.add_parameter({.id = kTriplet,
                             .name = "Triplet",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        // Whether the cycle is the window or one step. The fork between an LFO and
        // a sequencer; see brew/step.hpp.
        store.add_parameter({.id = kSpeedMode,
                             .name = "Rate Per Step",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = speed_mode_name});

        // ── The window into the eight steps ───────────────────────────────────
        store.add_parameter({.id = kStart,
                             .name = "Start",
                             .unit = "",
                             .range = {1.0f, static_cast<float>(kMaxSequencerSteps),
                                       1.0f, 1.0f},
                             .to_string = text::whole});
        store.add_parameter({.id = kLength,
                             .name = "Length",
                             .unit = "steps",
                             .range = {1.0f, static_cast<float>(kMaxSequencerSteps),
                                       8.0f, 1.0f},
                             .to_string = text::whole});
        store.add_parameter({.id = kEnd,
                             .name = "End",
                             .unit = "",
                             .range = {1.0f, static_cast<float>(kMaxSequencerSteps),
                                       static_cast<float>(kMaxSequencerSteps), 1.0f},
                             .to_string = text::whole});
        store.add_parameter({.id = kLengthMode,
                             .name = "Length Mode",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kLengthModeCount - 1),
                                       kStepDefaultLengthMode, 1.0f},
                             .to_string = length_mode_name});

        // ── The step's shape ─────────────────────────────────────────────────
        store.add_parameter({.id = kInterpolation,
                             .name = "Interpolation",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kInterpolationCount - 1),
                                       kStepDefaultInterp, 1.0f},
                             .to_string = interpolation_name});
        // Fraction of a step spent sliding into it. Zero is a hard edge, and
        // `Interpolation = Linear` is exactly this at 1.0.
        store.add_parameter({.id = kGlide,
                             .name = "Glide",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f},
                             .to_string = text::plain});
        // The fraction of each step that carries its value. At 1.0 nothing is
        // punched out and the gate never falls; below it, every step grows a
        // rising edge for an envelope generator to fire on.
        store.add_parameter({.id = kGate,
                             .name = "Gate",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.0f},
                             .to_string = text::fraction_percent});

        // ── The LFO's shaping controls, on the same cycle ─────────────────────
        store.add_parameter({.id = kPhaseDegrees,
                             .name = "Phase",
                             .unit = "deg",
                             .range = {-360.0f, 360.0f, 0.0f, 0.0f},
                             .to_string = text::degrees});
        // Bounds from the clock's own invertible interval, exactly as Sync and the
        // LFO take them. A knob that reached 0% or 100% would be turning through a
        // percent of dead travel at each end, because `swing_warp` clamps there.
        store.add_parameter({.id = kSwingPercent,
                             .name = "Swing",
                             .unit = "%",
                             .range = {static_cast<float>(kMinSwing * 100.0),
                                       static_cast<float>(kMaxSwing * 100.0),
                                       50.0f, 0.0f},
                             .to_string = text::percent1});
        store.add_parameter({.id = kSwingUnit,
                             .name = "Swing Sixteenths",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        // Warps the cycle's time axis. In `cycle` mode that stretches the early
        // steps of the window against the late ones; in `step` mode it warps each
        // step's own fraction, which is only visible through a glide.
        store.add_parameter({.id = kAsymmetry,
                             .name = "Asymmetry",
                             .unit = "",
                             .range = {0.05f, 0.95f, 0.5f, 0.0f},
                             .to_string = text::plain});
        store.add_parameter({.id = kLevelOffset,
                             .name = "Offset",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 0.0f, 0.0f},
                             .to_string = text::signed_plain});
        store.add_parameter({.id = kSmoothMs,
                             .name = "Smooth",
                             .unit = "ms",
                             .range = {-500.0f, 500.0f, 0.0f, 0.0f},
                             .to_string = text::smooth});

        // ── Output ───────────────────────────────────────────────────────────
        store.add_parameter({.id = kRange,
                             .name = "Range",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kRangeCount - 1),
                                       kStepDefaultRange, 1.0f},
                             .to_string = range_name});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.0f},
                             .to_string = text::fraction_percent});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});

        // ── Dither ───────────────────────────────────────────────────────────
        // A bounded random offset added to each step, keyed on the absolute step
        // index. At zero the pattern is exactly what the editor shows.
        store.add_parameter({.id = kRandom,
                             .name = "Random",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f},
                             .to_string = text::plain});
        store.add_parameter({.id = kSeed,
                             .name = "Seed",
                             .unit = "",
                             .range = {0.0f, 255.0f, 0.0f, 1.0f},
                             .to_string = text::whole});

        // ── The shift register ───────────────────────────────────────────────
        store.add_parameter({.id = kRegisterOn,
                             .name = "Register",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kRegisterLength,
                             .name = "Register Length",
                             .unit = "bits",
                             .range = {1.0f, static_cast<float>(kMaxRegisterBits), 8.0f,
                                       1.0f},
                             .to_string = text::whole});
        store.add_parameter({.id = kDacBits,
                             .name = "DAC Bits",
                             .unit = "bits",
                             .range = {1.0f, static_cast<float>(kMaxDacBits),
                                       static_cast<float>(kMaxDacBits), 1.0f},
                             .to_string = text::whole});
        // Signed: +1 never inverts the fed-back bit (a locked loop), 0 inverts it
        // half the time (never repeats), -1 always (a locked loop of twice the
        // length, alternately inverted).
        store.add_parameter({.id = kRandomness,
                             .name = "Randomness",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 1.0f, 0.0f},
                             .to_string = text::signed_plain});
        store.add_parameter({.id = kRotate,
                             .name = "Rotate",
                             .unit = "bits",
                             .range = {-static_cast<float>(kMaxRegisterBits - 1),
                                       static_cast<float>(kMaxRegisterBits - 1), 0.0f,
                                       1.0f},
                             .to_string = text::signed_whole});
        // Latched, not momentary: while it is on every bit shifted in is a one.
        // Automate it high for a single step and the two are the same operation —
        // and a latch is the only form of it a preset can hold.
        store.add_parameter({.id = kSetNext,
                             .name = "Set Next",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kAutoScale,
                             .name = "Auto Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f},
                             .to_string = text::on_off});
        store.add_parameter({.id = kDacScale,
                             .name = "Scale",
                             .unit = "",
                             .range = {0.05f, 8.0f, 1.0f, 0.0f},
                             .to_string = text::plain});

        // ── The input bus ────────────────────────────────────────────────────
        store.add_parameter({.id = kInputLeft,
                             .name = "Input Left",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kInputRoleCount - 1),
                                       kStepDefaultRole, 1.0f},
                             .to_string = input_role_name});
        store.add_parameter({.id = kInputRight,
                             .name = "Input Right",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kInputRoleCount - 1),
                                       kStepDefaultRole, 1.0f},
                             .to_string = input_role_name});
        store.add_parameter({.id = kSignalMode,
                             .name = "Signal Mode",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kInputModeCount - 1),
                                       kStepDefaultSignalMode, 1.0f},
                             .to_string = text::input_mode_name});

        // A gentle rising ramp, so an unedited instance does something visible on
        // the step editor rather than sitting flat and looking broken.
        for (int i = 0; i < kMaxSequencerSteps; ++i) {
            const float def =
                -1.0f + 2.0f * static_cast<float>(i) /
                            static_cast<float>(kMaxSequencerSteps - 1);
            store.add_parameter({.id = step_param(i),
                                 .name = "Step " + std::to_string(i + 1),
                                 .unit = "",
                                 .range = {-1.0f, 1.0f, def, 0.0f},
                                 .to_string = text::signed_plain});
        }

        // A binary ladder by default, so the DAC reads the register as a number.
        // Flatten a weight and the bit stops contributing; raise a low one and the
        // staircase reorders, which is the whole point of exposing them.
        for (int i = 0; i < kMaxDacBits; ++i) {
            store.add_parameter({.id = weight_param(i),
                                 .name = "DAC Weight " + std::to_string(i + 1),
                                 .unit = "",
                                 .range = {0.0f, 1.0f, default_dac_weight(i), 0.0f},
                                 .to_string = text::plain});
        }
    }

    /// Every control the editor must expose, in panel order. The step levels and
    /// the DAC weights are indexed families and are laid out from their own loops.
    [[nodiscard]] static constexpr std::array<ControlSpec, 28> controls() {
        return {{{kSyncMode, "Sync"},
                 {kSpeedHz, "Speed"},
                 {kMultiplier, "Mult"},
                 {kBeats, "Beats"},
                 {kDivisor, "Div"},
                 {kStart, "Start"},
                 {kLength, "Length"},
                 {kEnd, "End"},
                 {kLengthMode, "Bounds"},
                 {kInterpolation, "Interp"},
                 {kGlide, "Glide"},
                 {kGate, "Gate"},
                 {kPhaseDegrees, "Phase"},
                 {kSwingPercent, "Swing"},
                 {kAsymmetry, "Asym"},
                 {kLevelOffset, "Offset"},
                 {kSmoothMs, "Smooth"},
                 {kRandom, "Random"},
                 {kSeed, "Seed"},
                 {kOutputScale, "Out"},
                 {kRegisterLength, "Bits"},
                 {kDacBits, "DAC"},
                 {kRandomness, "Rand"},
                 {kRotate, "Rotate"},
                 {kDacScale, "Scale"},
                 {kInputLeft, "In L"},
                 {kInputRight, "In R"},
                 {kSignalMode, "Signal"}}};
    }

    /// The toggles, which the step editor and the bit lamps cannot show.
    [[nodiscard]] static constexpr std::array<ControlSpec, 8> toggles() {
        return {{{kTriplet, "Triplet"},
                 {kSwingUnit, "16ths"},
                 {kSpeedMode, "Per Step"},
                 {kInvert, "Invert"},
                 {kRegisterOn, "Register"},
                 {kSetNext, "Set Next"},
                 {kAutoScale, "Auto"},
                 {kRange, "Unipolar"}}};
    }

    void prepare(const format::PrepareContext&) override { hard_reset(); }

    /// The step editor, the bit lamps, eight rows of controls, three captions.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {380, 1034};
    }

    std::unique_ptr<view::View> create_view() override;

    // ── The settings the pure transfer reads ─────────────────────────────────

    [[nodiscard]] SyncMode sync_mode() const noexcept {
        return enum_from_param<SyncMode>(state().get_value(kSyncMode),
                                         kStepSyncModeCount);
    }

    [[nodiscard]] SpeedMode speed_mode() const noexcept {
        return speed_mode_from_param(state().get_value(kSpeedMode));
    }

    [[nodiscard]] Range range() const noexcept {
        return enum_from_param<Range>(state().get_value(kRange), kRangeCount);
    }

    [[nodiscard]] Interpolation interpolation() const noexcept {
        return enum_from_param<Interpolation>(state().get_value(kInterpolation),
                                              kInterpolationCount);
    }

    [[nodiscard]] InputRole input_role(std::size_t channel) const noexcept {
        return enum_from_param<InputRole>(
            state().get_value(channel == 0 ? kInputLeft : kInputRight), kInputRoleCount);
    }

    [[nodiscard]] InputMode signal_mode() const noexcept {
        return enum_from_param<InputMode>(state().get_value(kSignalMode),
                                          kInputModeCount);
    }

    [[nodiscard]] double hz() const noexcept {
        return free_hz(static_cast<double>(state().get_value(kSpeedHz)),
                       enum_from_param<Multiplier>(state().get_value(kMultiplier),
                                                   kMultiplierCount));
    }

    [[nodiscard]] double cycle_length_beats() const noexcept {
        return cycle_beats(static_cast<double>(state().get_value(kBeats)),
                           enum_from_param<NoteUnit>(state().get_value(kDivisor),
                                                     kNoteUnitCount),
                           as_toggle(state().get_value(kTriplet)));
    }

    [[nodiscard]] Swing current_swing() const noexcept {
        return {.unit_beats = as_toggle(state().get_value(kSwingUnit)) ? kSixteenthBeats
                                                                       : kEighthBeats,
                .amount = static_cast<double>(state().get_value(kSwingPercent)) / 100.0};
    }

    /// `Phase`, in cycles. The same control the LFO has, and it means the same
    /// thing: where in the cycle the pattern's first step begins.
    [[nodiscard]] double phase_offset() const noexcept {
        return static_cast<double>(state().get_value(kPhaseDegrees)) / 360.0;
    }

    [[nodiscard]] int start_step() const noexcept {
        return std::clamp(static_cast<int>(std::lround(state().get_value(kStart))), 1,
                          kMaxSequencerSteps) -
               1;
    }

    [[nodiscard]] int end_step() const noexcept {
        return std::clamp(static_cast<int>(std::lround(state().get_value(kEnd))), 1,
                          kMaxSequencerSteps) -
               1;
    }

    [[nodiscard]] int length() const noexcept {
        return std::clamp(static_cast<int>(std::lround(state().get_value(kLength))), 1,
                          kMaxSequencerSteps);
    }

    /// How many of the eight steps the window covers, under whichever mode names it.
    [[nodiscard]] int window() const noexcept {
        return window_length(enum_from_param<LengthMode>(state().get_value(kLengthMode),
                                                         kLengthModeCount),
                             start_step(), length(), end_step());
    }

    [[nodiscard]] RegisterSettings register_settings() const noexcept {
        return {.length = static_cast<int>(std::lround(state().get_value(kRegisterLength))),
                .randomness = static_cast<double>(state().get_value(kRandomness)),
                .seed = static_cast<std::uint32_t>(state().get_value(kSeed)),
                .set_next = as_toggle(state().get_value(kSetNext))};
    }

    [[nodiscard]] DacSettings dac_settings() const noexcept {
        DacSettings d;
        d.bits = static_cast<int>(std::lround(state().get_value(kDacBits)));
        d.rotate = static_cast<int>(std::lround(state().get_value(kRotate)));
        for (int i = 0; i < kMaxDacBits; ++i)
            d.weights[static_cast<std::size_t>(i)] = state().get_value(weight_param(i));
        d.automatic_scale = as_toggle(state().get_value(kAutoScale));
        d.scale = state().get_value(kDacScale);
        return d;
    }

    [[nodiscard]] bool register_on() const noexcept {
        return as_toggle(state().get_value(kRegisterOn));
    }

    /// How much of each step carries its value; the rest is silent.
    [[nodiscard]] double gate_fraction() const noexcept {
        return static_cast<double>(state().get_value(kGate));
    }

    // ── The pure transfer, shared by process(), the editor, and the tests ─────

    /// The bipolar level the pattern holds at an absolute step, before the glide,
    /// the gate, and the output stage.
    ///
    /// Takes the caller's register rather than owning one, because the register's
    /// cache is mutable and the editor draws on a different thread from the one
    /// `process()` runs on. Two readers, two caches, one pure function.
    [[nodiscard]] float level_at(std::int64_t abs_step, ShiftRegister& reg) const noexcept {
        float base;
        if (register_on()) {
            const RegisterSettings rs = register_settings();
            // The DAC reads unipolar; the pattern speaks bipolar everywhere until
            // `Range` has its say at the jack.
            base = 2.0f * dac_value(reg.at(abs_step, rs), rs.length, dac_settings()) - 1.0f;
        } else {
            base = state().get_value(step_param(pattern_index(abs_step, start_step(), window())));
        }
        return step_value(base, abs_step, state().get_value(kRandom),
                          static_cast<std::uint32_t>(state().get_value(kSeed)));
    }

    /// The voltage the CV jack carries at a fractional step position.
    [[nodiscard]] float value_at_position(double position, ShiftRegister& reg,
                                          float input = 0.0f) const noexcept {
        const std::int64_t abs = absolute_step(position);
        const double frac = step_fraction(position);
        const float v = glide_toward(level_at(abs - 1, reg), level_at(abs, reg), frac,
                                     effective_glide(interpolation(),
                                                     static_cast<double>(state().get_value(kGlide))));
        // `Range` remaps what a step knob *means* — full scale, or its upper half.
        // `Gate` silences the jack for the tail of the step. So the range is applied
        // to the programmed level and the gate to the result: a gap is zero volts in
        // either range, which is the only value a hardware gate input reads as low.
        // Gating the level instead would put a unipolar gap at mid-scale, high
        // enough to hold an envelope open for the whole pattern.
        const float ranged = apply_range(v, range());
        const float gated = step_gated(ranged, frac, gate_fraction());
        // `Offset` is a DC shift of the whole waveform, gaps included.
        const float shifted = gated + state().get_value(kLevelOffset);
        return resolve_output(apply_input(signal_mode(), shifted, input), scale(),
                              inverted());
    }

    /// Convenience for the editor and the tests: the value at a cycle count.
    [[nodiscard]] float value_at_cycles(double cycles, ShiftRegister& reg) const noexcept {
        return value_at_position(position_at_cycles(cycles), reg);
    }

    /// Where in the pattern a cycle count lands, after `Phase`, `Asymmetry`, and
    /// whichever of the two things a cycle currently means.
    [[nodiscard]] double position_at_cycles(double cycles) const noexcept {
        const double c = cycles + phase_offset();
        // The warp is monotone within a cycle and pinned at its ends, so the cycle
        // boundaries stay put and only the interior moves.
        const double warped =
            std::floor(c) + warp_phase(c, static_cast<double>(state().get_value(kAsymmetry)));
        return step_position(warped, speed_mode(), window());
    }

    /// Which step of the pattern is playing, or -1 when nothing is. The editor
    /// highlights it; a negative value hides the highlight rather than lying.
    [[nodiscard]] int display_step() const noexcept {
        return display_step_.load(std::memory_order_relaxed);
    }

    /// The register's bits as the editor's lamps last saw them.
    [[nodiscard]] std::uint64_t display_bits() const noexcept {
        return display_bits_.load(std::memory_order_relaxed);
    }

    // ── The audio callback ───────────────────────────────────────────────────

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels = output.num_channels();
        const int frames = static_cast<int>(output.num_samples());
        if (channels == 0 || frames <= 0) return;

        float* cv = output.channel_ptr(0);
        float* gate = channels > 1 ? output.channel_ptr(1) : nullptr;

        // Bypass stops driving the patch: hold both outputs at zero rather than
        // freeze them at whatever level the pattern happened to reach.
        if (ctx.is_bypassed) {
            for (std::size_t c = 0; c < channels; ++c)
                if (float* dst = output.channel_ptr(c))
                    for (int n = 0; n < frames; ++n) dst[n] = 0.0f;
            smooth_.reset(0.0f);
            display_step_.store(-1, std::memory_order_relaxed);
            was_playing_ = ctx.is_playing;
            return;
        }

        if (ctx.reset_requested) hard_reset();

        const Transport transport = transport_from(ctx);
        const ClockSettings settings = clock_settings(ctx.is_playing);
        clock_.begin_block(settings, transport);

        const SyncMode mode = settings.mode;
        const bool paused = sync_pauses_for_trigger(mode);
        const double sr = sr_of(ctx);
        const float ms = state().get_value(kSmoothMs);
        const double gate_open = gate_fraction();
        const float sc = scale();
        const bool inv = inverted();

        // Which input channel plays which part. A channel can be two things at
        // once only by being patched twice, which is what a mult is for.
        const std::array<InputRole, 2> roles{input_role(0), input_role(1)};
        const std::size_t shared = std::min<std::size_t>(2, input.num_channels());

        std::int64_t last_abs = 0;
        for (int n = 0; n < frames; ++n) {
            const double dn = static_cast<double>(n);

            float signal = 0.0f;
            for (std::size_t c = 0; c < shared; ++c) {
                const float* src = input.channel_ptr(c);
                const float v = src ? src[n] : 0.0f;
                switch (roles[c]) {
                    case InputRole::off: break;
                    case InputRole::signal: signal += v; break;
                    case InputRole::reset:
                        if (edge_[c].process(v)) {
                            shift_ = raw_position(settings, transport, dn);
                            allowed_steps_ = 1;
                        }
                        break;
                    case InputRole::trigger:
                        if (edge_[c].process(v)) {
                            // Re-anchor so the released step *starts* here rather
                            // than being skipped through — the clock has run on
                            // while the pattern was waiting.
                            if (paused)
                                shift_ = raw_position(settings, transport, dn) -
                                         static_cast<double>(allowed_steps_);
                            ++allowed_steps_;
                        }
                        break;
                }
            }

            double pos = raw_position(settings, transport, dn) - shift_;
            if (paused) {
                const double ceiling = static_cast<double>(allowed_steps_);
                // Hold at the *end* of the last released step, not at the start of
                // the next: the step the pattern is waiting on is still sounding.
                if (pos >= ceiling) pos = std::nextafter(ceiling, 0.0);
            }

            const float raw = value_at_position(pos, register_, signal);
            if (cv) cv[n] = smooth_.process(raw, ms, sr);
            if (gate) {
                const bool on = step_gate_open(step_fraction(pos), gate_open);
                gate[n] = resolve_output(on ? kFullScale : 0.0f, sc, inv);
            }
            last_abs = absolute_step(pos);
        }

        // An output channel with no jack behind it is silent, not uninitialized.
        for (std::size_t c = 2; c < channels; ++c)
            if (float* dst = output.channel_ptr(c))
                for (int n = 0; n < frames; ++n) dst[n] = 0.0f;

        clock_.end_block(settings, transport, frames);
        was_playing_ = ctx.is_playing;

        display_step_.store(pattern_index(last_abs, start_step(), window()),
                            std::memory_order_relaxed);
        display_bits_.store(register_on() ? register_.at(last_abs, register_settings())
                                          : 0ULL,
                            std::memory_order_relaxed);
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            audio::BufferView<const float> empty;
            const auto* in = audio.main_input();
            process(*out, in ? *in : empty, midi_in, midi_out, ctx);
        }
    }

private:
    [[nodiscard]] static double sr_of(const format::ProcessContext& ctx) noexcept {
        return ctx.sample_rate > 0.0 ? ctx.sample_rate : 48000.0;
    }

    [[nodiscard]] Transport transport_from(const format::ProcessContext& ctx) noexcept {
        const double sr = sr_of(ctx);
        Transport t;
        t.position_beats = ctx.position_beats;
        t.position_seconds = static_cast<double>(ctx.position_samples) / sr;
        t.beats_per_sample = beats_per_sample(ctx.tempo_bpm, sr);
        t.sample_rate = sr;
        t.playing = ctx.is_playing;
        t.play_edge = (ctx.is_playing && !was_playing_) || ctx.transport_started;
        return t;
    }

    /// Swing warps a beat timeline. With the playhead parked there is nothing to
    /// push late, so it is dropped rather than applied to a still frame.
    [[nodiscard]] ClockSettings clock_settings(bool playing) const noexcept {
        return {.mode = sync_mode(),
                .hz = hz(),
                .cycle_beats = cycle_length_beats(),
                .swing = playing ? current_swing() : Swing{}};
    }

    /// The step position the clock alone would produce, before a reset or a
    /// trigger hold has moved it.
    [[nodiscard]] double raw_position(const ClockSettings& s, const Transport& t,
                                      double n) const noexcept {
        return position_at_cycles(clock_.cycles_at(s, t, n));
    }

    [[nodiscard]] float scale() const noexcept { return state().get_value(kOutputScale); }
    [[nodiscard]] bool inverted() const noexcept {
        return as_toggle(state().get_value(kInvert));
    }

    void hard_reset() noexcept {
        clock_.reset();
        register_.reset();
        smooth_.reset(0.0f);
        for (auto& e : edge_) e.reset();
        shift_ = 0.0;
        allowed_steps_ = 1;
        was_playing_ = false;
    }

    PhaseClock clock_{};
    ShiftRegister register_{};
    Smoother smooth_{};
    std::array<TriggerDetector, 2> edge_{};

    /// How far the reset and trigger inputs have displaced the pattern's origin,
    /// in step positions. Zero for every patch that leaves the input bus alone,
    /// which is why those patches stay a pure function of the timeline.
    double shift_ = 0.0;
    /// How many steps the trigger input has released. Only read in the two modes
    /// that pause; `1` lets the pattern play step zero and then wait.
    std::int64_t allowed_steps_ = 1;

    bool was_playing_ = false;

    std::atomic<int> display_step_{-1};
    std::atomic<std::uint64_t> display_bits_{0};
};

inline std::unique_ptr<format::Processor> create_step() {
    return std::make_unique<StepProcessor>();
}

}  // namespace pulp::examples::brew
