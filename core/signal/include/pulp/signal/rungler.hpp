#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>

#include <algorithm>
#include <cstdint>

namespace pulp::signal {

// ── Rungler ───────────────────────────────────────────────────────────────

/// The Hordijk shift-register / DAC chaos sequencer.
///
/// *(lineage — informative)* The **rungler** is Rob Hordijk's signature circuit
/// from the Blippoo Box: an N-stage shift register whose serial input is fed
/// back from the register itself, clocked by an oscillator, with a few register
/// bits driving a small **DAC** whose stepped voltage modulates the oscillators
/// — a loop that produces smooth, quasi-periodic chaos, roughly predictable yet
/// never exactly repeating [Rob Hordijk, "The Blippoo Box: A Chaotic Electronic
/// Music Instrument, Bent by Design," *Leonardo Music Journal* 19 (2009),
/// 35–43, MIT Press]. The *topology* is cited; the **specific tap positions are
/// a design parameter**, because the paper describes the concept rather than one
/// canonical wiring and no citable exact tap map exists. Those are our
/// engineering choices, not cited constants.
///
/// **Why it sounds "smoothly chaotic."** With pure self-feedback the register is
/// a finite-state machine over `2^N` states, so the output is strictly
/// *periodic* — but the period can be very long and the orbit visits its DAC
/// levels in an order with no small repeating unit, which the ear reads as
/// evolving, roughly predictable, never quite looping. XOR-ing an external data
/// bit perturbs the state each clock, lengthening and reshaping the orbit, so a
/// slowly changing input voltage *steers* the chaos without ever making it
/// random. The compositional value is deterministic long-form variation:
/// controllable, but not memorizable.
///
/// **The bound is provable, not measured** (series law 1 + law 8). The only
/// feedback is a 1-bit XOR into the register; there is no gain-carrying analog
/// nonlinearity, so no small-signal-gain compensation applies. The output is a
/// `D`-bit DAC code mapped affinely onto `[−range_v, +range_v]`, so
/// `|y| ≤ range_v` **by construction** for any clock or data sequence
/// whatsoever. That is the invariant a registry `worst_case_gain` cites, and the
/// suite asserts it over adversarial data rather than typical data.
///
/// **No oversampling, no smoothing.** The stepped hold *is* the sound. There is
/// no continuous nonlinearity here to alias, so band-limiting would only remove
/// the intended edge; a caller who wants the steps softened puts a
/// `SlewLimiterT` downstream (the "drunken glide" patch).
///
/// **The all-zero register is an absorbing state.** With XOR feedback, `0 ⊕ 0`
/// is 0 forever, so a seed pattern of 0 produces a constant `−range_v`. That is
/// left as-is rather than remapped the way `Xorshift32` remaps a zero seed: the
/// seed pattern is a musical parameter whose neighbours must stay meaningful,
/// and silently substituting a different pattern for one value would make it
/// non-monotone. Enabling `external_data` kicks the register out of the state on
/// the first `data_in` bit that is 1.
///
/// **USE.** *Generative pitch source* — rungler CV → `QuantizeScaleT` →
/// oscillator: the archetypal self-playing patch, always in key, never quite
/// repeating. *Timbral chaos* — into a filter cutoff or a wavefolder depth for
/// burbling, semi-predictable motion. *Cross-modulation* — clock the rungler
/// from an oscillator whose pitch the rungler modulates (caller-wired) for the
/// Blippoo's audio-rate loop. *Tamed* — into a `SlewLimiterT` for a wandering
/// portamento line.
///
/// RT contract: as the header. Integer bit operations only.
template <typename SampleType = float>
class RunglerT {
public:
    /// Shift-register length.
    /// [design parameter] default 8, range 4 .. 16.
    static constexpr int kMinBits = 4;
    static constexpr int kMaxBits = 16;
    static constexpr int kDefaultBits = 8;

    /// DAC width — `2^D` output levels. D = 3 gives the classic eight steps.
    /// [design parameter] default 3, range 1 .. 4.
    static constexpr int kMinDacBits = 1;
    static constexpr int kMaxDacBits = 4;
    static constexpr int kDefaultDacBits = 3;

    /// Initial register pattern, restored by `reset()` *and* by a reset edge.
    /// [design parameter] default 0b10110100 (180), range 0 .. 65535.
    static constexpr std::uint32_t kSeedPattern = 0b10110100u;

    /// Register index XOR-ed with the last stage to form the serial input.
    /// [design parameter] default 0, range 0 .. N − 2.
    static constexpr int kFeedbackTap = 0;

    /// Output span in volts: the DAC covers `[−range_v, +range_v]`.
    /// [design parameter] default 2 V, range 0.5 .. 5 V.
    static constexpr double kRangeV = 2.0;

    /// Ceiling on the output span. Two orders past the documented range — no
    /// modular standard exceeds ±15 V — so it only catches a nonsense value.
    /// The bound is what makes `|y| <= range_v` a FINITE guarantee.
    /// [design parameter] default 1000 V ceiling, range 15 .. 100000 V.
    static constexpr double kMaxRangeV = 1000.0;

    RunglerT() { load_seed(); }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
    }

    /// Changing the register length reloads the seed pattern. A shift register
    /// whose length changes mid-orbit has no meaningful "same state" to keep —
    /// truncating drops bits and extending invents them — so the deterministic
    /// answer is to start the new length from the seed rather than from a
    /// silently reinterpreted bit pattern.
    void set_reg_bits(int n) {
        bits_ = std::clamp(n, kMinBits, kMaxBits);
        clamp_config();
        load_seed();
    }
    int reg_bits() const { return bits_; }

    void set_dac_bits(int d) {
        dac_bits_ = std::clamp(d, kMinDacBits, kMaxDacBits);
        clamp_config();
        refresh_output();
    }
    int dac_bits() const { return dac_bits_; }

    void set_feedback_tap(int tap) {
        tap_ = tap;
        clamp_config();
    }
    int feedback_tap() const { return tap_; }

    void set_range_v(double volts) {
        range_v_ = seq_detail::clamp_finite(volts, 0.0, kMaxRangeV);
        refresh_output();
    }
    double range_v() const { return range_v_; }

    void set_external_data(bool on) { external_data_ = on; }
    bool external_data() const { return external_data_; }

    void set_seed_pattern(std::uint32_t pattern) {
        seed_pattern_ = pattern;
        load_seed();
    }
    std::uint32_t seed_pattern() const { return seed_pattern_; }

    /// Verb 2 — and the one documented exception to "continuous outputs hold":
    /// the register goes back to the seed pattern and the output steps to
    /// `DAC(seed)` immediately, so a wandering line can be re-pinned live.
    void apply_reset_edge() { load_seed(); }

    void reset() { load_seed(); }

    static constexpr int latency_samples() { return 0; }

    /// Current register contents, bit `i` at bit position `i`.
    std::uint32_t register_bits() const { return reg_; }
    /// Current DAC code in `0 .. 2^D − 1`.
    int dac_code() const { return static_cast<int>(reg_ & dac_mask()); }
    SampleType value() const { return out_; }

    /// Advances one sample. `data_in` is XOR-ed into the serial input when
    /// `external_data` is enabled — the paper's "bent by design" input, most
    /// naturally driven from a `ComparatorT` (kit) on any signal in the patch.
    SampleType process(bool run_high, bool reset_edge, bool clock_edge, bool data_in = false) {
        if (reset_edge) apply_reset_edge();
        if (!run_high) return out_;
        if (!clock_edge) return out_;

        const std::uint32_t last = (reg_ >> (bits_ - 1)) & 1u;
        const std::uint32_t tap = (reg_ >> tap_) & 1u;
        std::uint32_t new_bit = last ^ tap;
        if (external_data_ && data_in) new_bit ^= 1u;

        reg_ = ((reg_ << 1) | new_bit) & reg_mask();
        refresh_output();
        return out_;
    }

private:
    std::uint32_t reg_mask() const { return (1u << bits_) - 1u; }
    std::uint32_t dac_mask() const { return (1u << dac_bits_) - 1u; }

    void clamp_config() {
        if (dac_bits_ > bits_) dac_bits_ = bits_;
        tap_ = std::clamp(tap_, 0, std::max(bits_ - 2, 0));
    }

    void load_seed() {
        clamp_config();
        reg_ = seed_pattern_ & reg_mask();
        refresh_output();
    }

    void refresh_output() {
        const double levels = static_cast<double>(dac_mask());
        const double k = static_cast<double>(reg_ & dac_mask());
        const double unit = levels > 0.0 ? (2.0 * k / levels - 1.0) : 0.0;
        out_ = static_cast<SampleType>(range_v_ * unit);
    }

    double sample_rate_ = 44100.0;
    int bits_ = kDefaultBits;
    int dac_bits_ = kDefaultDacBits;
    int tap_ = kFeedbackTap;
    double range_v_ = kRangeV;
    bool external_data_ = false;
    std::uint32_t seed_pattern_ = kSeedPattern;

    std::uint32_t reg_ = 0;
    SampleType out_ = SampleType{0};
};

using Rungler = RunglerT<float>;
using Rungler64 = RunglerT<double>;

}  // namespace pulp::signal
