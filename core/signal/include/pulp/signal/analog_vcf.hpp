#pragma once

// Measured voicing layer for the shared four-pole OTA cascade engine.
//
// WHY THIS EXISTS
// The panel controls on classic polysynth and monosynth filters are not generic
// Hz/Q controls. Their cutoff laws, resonance tapers, loop ceilings, passband
// compensation and saturation rails are part of the sound. AnalogVcfT keeps
// those measured laws together while OtaCascadeFilterT owns the reusable DSP
// topology. This boundary also keeps host/catalog code out of the signal layer:
// Forge and other hosts expose knob-domain controls through a separate Pulp
// CustomNodeType adapter.
//
// Cutoff tables interpolate in log-frequency space. Resonance, compensation,
// headroom, cross-modulation, makeup and drift tables interpolate linearly in
// their measured knob domains. Cutoff modulation first moves corner frequency
// in octaves, then inverts the table so cutoff-dependent character follows the
// equivalent panel position rather than remaining frozen at the unmodulated
// knob.
//
// CALIBRATION PROVENANCE, AND WHY THE ROWS ARE READ DIFFERENTLY
// The tables here are the measured data and are authoritative. The *acceptance
// criteria* that once accompanied them were not: they were written by
// summarizing a calibration pipeline at a single point in time while the
// pipeline was still moving, and the prose was never revised as the tables and
// laws were. Two consequences survive in test_analog_vcf.cpp and are worth
// knowing before touching either:
//
//   * Peak-height emphasis is read on the stimulus harmonic grid within an
//     octave of the ring, not as a global spectral maximum. The global form
//     reports the post-output cross-modulation sideband, or an inter-harmonic
//     line riding a resonance-zero valley -- noise over noise. The upstream
//     Juno calibration hit that same artifact and added a floor guard; the
//     acceptance prose predates the fix.
//   * The Juno and Jupiter-8 columns were refit on the canonical saw render, so
//     they are read there. The Prophet-5 taper was fitted analytically through
//     the ladder relation, so its two sub-oscillation rows are read with a
//     continuous swept probe instead -- its ring at cutoff knob 0.55 falls
//     between two harmonics of the 110 Hz stimulus and the saw grid cannot
//     resolve it. Above the self-oscillation crossing (res knob ~0.584) the
//     small-signal probe stops meaning anything and the saw recipe takes over.
//
// The Prophet-5 rising-rail and cross-mod laws below postdate the peak-height
// column that was originally published with them, and no re-sweep of the
// composite voicing followed. The engine reproduces the ring growth those laws
// were built from; the amended targets in the tests are law-derived.

#include <pulp/signal/ota_cascade_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace pulp::signal {

/// Measured panel voicings backed by a four-pole nonlinear OTA cascade.
///
/// The default state is a Juno voicing at 44.1 kHz and 2x oversampling, with
/// cutoff 0.5 (262 Hz requested corner), zero resonance, zero dB drive, and no
/// coefficient smoothing. Processing and configuration use fixed storage and
/// allocate no memory. Configuration methods that clear DSP history say so
/// explicitly below.
/// @tparam SampleType Floating-point sample scalar; normally float or double.
template <typename SampleType = float>
class AnalogVcfT {
public:
    /// Selects the complete measured cutoff, resonance, headroom, compensation,
    /// saturation, cross-modulation, makeup, and drift law.
    enum class Voicing {
        /// Roland Juno-style IR3109 voicing; the default.
        juno,
        /// Roland Jupiter-8-style IR3109 voicing.
        jupiter,
        /// Sequential Prophet-5-style SSM/CEM voicing.
        prophet5,
        /// Minimoog-style transistor-ladder voicing.
        minimoog,
    };

    /// Constructs the default configuration and clears all DSP history.
    AnalogVcfT() noexcept {
        update_voicing();
        engine_.reset();
    }

    /// Sets the base sample rate in Hz, with values below 64 Hz clamped to 64.
    /// Reapplies the active voicing and clears filter, oversampler, and drift
    /// history, even when the supplied rate equals the current rate.
    void set_sample_rate(double sample_rate) noexcept {
        engine_.set_sample_rate(sample_rate);
        update_voicing();
        engine_.reset();
    }

    /// Sets the oversampling factor to 1, 2, 4, 8, or 16.
    /// Invalid factors select 2x through the underlying engine. Every call
    /// reapplies the active voicing and clears DSP history.
    void set_oversampling(int factor) noexcept {
        engine_.set_oversampling(factor);
        update_voicing();
        engine_.reset();
    }

    /// Selects a measured voicing and clears DSP history when it changes.
    /// Passing the active voicing is a no-op.
    void set_voicing(Voicing voicing) noexcept {
        if (voicing_ == voicing) return;
        voicing_ = voicing;
        update_voicing();
        engine_.reset();
    }

    /// Sets panel cutoff and modulation without clearing DSP history.
    /// @param knob01 Panel position, clamped to [0, 1].
    /// @param modulation_octaves Pitch-domain offset in octaves, clamped to
    ///        [-16, 16]; defaults to zero.
    void set_cutoff(SampleType knob01, SampleType modulation_octaves = 0) noexcept {
        const double cutoff = std::clamp(static_cast<double>(knob01), 0.0, 1.0);
        const double modulation =
            std::clamp(static_cast<double>(modulation_octaves), -16.0, 16.0);
        if (cutoff_knob_ == cutoff && cutoff_modulation_octaves_ == modulation)
            return;
        cutoff_knob_ = cutoff;
        cutoff_modulation_octaves_ = modulation;
        update_voicing();
    }

    /// Sets the panel resonance position, clamped to [0, 1].
    /// The measured voicing law is updated without clearing DSP history.
    void set_resonance(SampleType knob01) noexcept {
        const double resonance = std::clamp(static_cast<double>(knob01), 0.0, 1.0);
        if (resonance_knob_ == resonance) return;
        resonance_knob_ = resonance;
        update_voicing();
    }

    /// Sets input drive in dB; the underlying engine clamps it to [-96, 72] dB.
    /// This updates the smoothed target without clearing DSP history.
    void set_drive_db(SampleType drive_db) noexcept {
        const double drive = static_cast<double>(drive_db);
        if (drive_db_ == drive) return;
        drive_db_ = drive;
        engine_.set_drive_db(drive_db_);
    }

    /// Updates all sample-accurate controls as one transaction.
    /// @param cutoff_knob01 Panel cutoff, clamped to [0, 1].
    /// @param modulation_octaves Cutoff offset in octaves, clamped to [-16, 16].
    /// @param resonance_knob01 Panel resonance, clamped to [0, 1].
    /// @param drive_db Input drive in dB, clamped by the engine to [-96, 72].
    ///
    /// This is equivalent to the individual setters but avoids evaluating the
    /// measured laws twice when cutoff and resonance arrive together. It does
    /// not clear DSP history.
    void set_parameters(SampleType cutoff_knob01, SampleType modulation_octaves,
                        SampleType resonance_knob01, SampleType drive_db) noexcept {
        const double cutoff =
            std::clamp(static_cast<double>(cutoff_knob01), 0.0, 1.0);
        const double modulation =
            std::clamp(static_cast<double>(modulation_octaves), -16.0, 16.0);
        const double resonance =
            std::clamp(static_cast<double>(resonance_knob01), 0.0, 1.0);
        const double drive = static_cast<double>(drive_db);
        const bool calibration_changed =
            cutoff_knob_ != cutoff ||
            cutoff_modulation_octaves_ != modulation ||
            resonance_knob_ != resonance;
        const bool drive_changed = drive_db_ != drive;
        if (!calibration_changed && !drive_changed) return;

        cutoff_knob_ = cutoff;
        cutoff_modulation_octaves_ = modulation;
        resonance_knob_ = resonance;
        drive_db_ = drive;
        // Drive is independent of every measured panel law. A drive-only macro
        // can move every sample, so do not make it pay for cutoff-table
        // inversion, voicing-law interpolation and tan() coefficient work.
        if (calibration_changed)
            update_voicing();
        else
            engine_.set_drive_db(drive_db_);
    }

    /// Sets coefficient smoothing time in milliseconds.
    /// Negative values select zero, so targets are reached on the next processed
    /// sample. This does not clear DSP history.
    void set_smoothing_time_ms(double milliseconds) noexcept {
        engine_.set_smoothing_time_ms(milliseconds);
    }

    /// Processes one base-rate sample and advances filter, oversampling,
    /// smoothing, and deterministic drift state.
    SampleType process(SampleType input) noexcept {
        return engine_.process(input);
    }

    /// Processes @p num_samples in place at the base sample rate.
    /// A null @p buffer or non-positive count is a no-op.
    void process(SampleType* buffer, int num_samples) noexcept {
        engine_.process(buffer, num_samples);
    }

    /// Clears filter and oversampler history, restores deterministic drift
    /// state, and snaps smoothed values to their current targets.
    /// Control settings, voicing, sample rate, and oversampling are preserved.
    void reset() noexcept {
        engine_.reset();
    }

    /// Returns the current fixed oversampling delay in base-rate samples.
    int latency_samples() const noexcept {
        return engine_.latency_samples();
    }

    /// Returns fixed base-rate latency for an oversampling factor.
    /// The mapping is 1x/2x/4x/8x/16x to 0/32/48/56/60 samples. Returns -1 for
    /// any invalid factor; unlike set_oversampling(), this does not apply the
    /// invalid-factor fallback.
    static constexpr int latency_samples_for_oversampling(int factor) noexcept {
        return OtaCascadeFilterT<SampleType>::latency_samples_for_oversampling(factor);
    }

    /// Returns the table-derived requested corner in Hz.
    /// This is panel intent, not necessarily the realized -3 dB corner: the
    /// derived engine pole is clamped to [20 Hz, 0.45 * sample_rate]. Code that
    /// must match rendered audio should measure the realized response.
    double cutoff_hz() const noexcept {
        return corner_hz_;
    }

    /// Converts panel cutoff to the requested corner in Hz without an instance.
    /// @param voicing Measured cutoff table to use.
    /// @param knob01 Panel position, clamped to [0, 1].
    /// @param modulation_octaves Pitch-domain offset in octaves, clamped to
    ///        [-16, 16]; defaults to zero.
    /// @return The requested, sample-rate-independent corner in Hz. Engine pole
    ///         clamping is intentionally not represented.
    static double requested_cutoff_hz_for(Voicing voicing, double knob01,
                                          double modulation_octaves = 0.0) noexcept {
        const double cutoff = std::clamp(knob01, 0.0, 1.0);
        const double modulation = std::clamp(modulation_octaves, -16.0, 16.0);
        return log_frequency_table(cutoff, calibration_tables(voicing).cutoff) *
               std::exp2(modulation);
    }

    /// Returns the active oversampling factor: 1, 2, 4, 8, or 16.
    int oversampling() const noexcept {
        return engine_.oversampling();
    }

    /// Returns the active measured voicing.
    Voicing voicing() const noexcept {
        return voicing_;
    }

private:
    struct TableView {
        std::span<const double> knots;
        std::span<const double> values;
    };

    struct CalibrationTables {
        TableView cutoff;
        TableView resonance;
    };

    static double linear_table(double knob, TableView table) noexcept {
        const auto knots = table.knots;
        const auto values = table.values;
        if (knob <= knots.front())
            return values.front();
        if (knob >= knots.back())
            return values.back();
        for (std::size_t i = 1; i < knots.size(); ++i) {
            if (knob <= knots[i]) {
                const double fraction = (knob - knots[i - 1]) / (knots[i] - knots[i - 1]);
                return values[i - 1] + fraction * (values[i] - values[i - 1]);
            }
        }
        return values.back();
    }

    static double log_frequency_table(double knob, TableView table) noexcept {
        const auto knots = table.knots;
        const auto values = table.values;
        if (knob <= knots.front())
            return values.front();
        if (knob >= knots.back())
            return values.back();
        for (std::size_t i = 1; i < knots.size(); ++i) {
            if (knob <= knots[i]) {
                const double fraction = (knob - knots[i - 1]) / (knots[i] - knots[i - 1]);
                return values[i - 1] * std::pow(values[i] / values[i - 1], fraction);
            }
        }
        return values.back();
    }

    static double invert_log_frequency_table(double frequency, TableView table) noexcept {
        const auto knots = table.knots;
        const auto values = table.values;
        if (frequency <= values.front())
            return knots.front();
        if (frequency >= values.back())
            return knots.back();
        for (std::size_t i = 1; i < knots.size(); ++i) {
            if (frequency <= values[i]) {
                const double fraction =
                    std::log(frequency / values[i - 1]) / std::log(values[i] / values[i - 1]);
                return knots[i - 1] + fraction * (knots[i] - knots[i - 1]);
            }
        }
        return knots.back();
    }

    static CalibrationTables calibration_tables(Voicing voicing) noexcept {
        switch (voicing) {
        case Voicing::juno:
            return {{kJunoCutoffKnots, kJunoCutoffHz}, {kJunoResonanceKnots, kJunoResonanceValues}};
        case Voicing::jupiter:
            return {{kJupiterCutoffKnots, kJupiterCutoffHz},
                    {kJupiterResonanceKnots, kJupiterResonanceValues}};
        case Voicing::prophet5:
            return {{kProphetCutoffKnots, kProphetCutoffHz},
                    {kProphetResonanceKnots, kProphetResonanceValues}};
        case Voicing::minimoog:
            return {{kMinimoogCutoffKnots, kMinimoogCutoffHz},
                    {kMinimoogResonanceKnots, kMinimoogResonanceValues}};
        }
        return {{kJunoCutoffKnots, kJunoCutoffHz}, {kJunoResonanceKnots, kJunoResonanceValues}};
    }

    void update_voicing() noexcept {
        const CalibrationTables tables = calibration_tables(voicing_);
        corner_hz_ = requested_cutoff_hz_for(
            voicing_, cutoff_knob_, cutoff_modulation_octaves_);
        const double equivalent_knob =
            invert_log_frequency_table(corner_hz_, tables.cutoff);
        const double resonance =
            linear_table(resonance_knob_, tables.resonance);

        engine_.set_mode(OtaCascadeFilterT<SampleType>::Mode::lowpass24);
        engine_.set_bias(0.0);
        engine_.set_drive_db(drive_db_);

        switch (voicing_) {
        case Voicing::juno:
            engine_.set_k_max(4.30);
            engine_.set_resonance(resonance);
            engine_.set_compensation(0.21);
            engine_.set_saturation_headroom(1.0);
            engine_.set_output_gain(1.0);
            engine_.set_cross_modulation(0.0, 2.0);
            engine_.set_drift(0.0, 0.0);
            engine_.set_pole_frequency(corner_hz_ / 0.4346);
            break;

        case Voicing::jupiter:
            engine_.set_k_max(3.53);
            engine_.set_resonance(resonance);
            engine_.set_compensation(0.14);
            engine_.set_saturation_headroom(1.5);
            engine_.set_output_gain(1.0);
            engine_.set_cross_modulation(0.0, 2.0);
            engine_.set_drift(0.0, 0.0);
            engine_.set_pole_frequency(corner_hz_ / 0.4346);
            break;

        case Voicing::prophet5: {
            const double compensation =
                linear_table(resonance_knob_, {kProphetLawKnots, kProphetCompensation});
            const double headroom =
                linear_table(resonance_knob_, {kProphetLawKnots, kProphetHeadroom});
            const double cross_mod =
                linear_table(resonance_knob_, {kProphetLawKnots, kProphetCrossMod});
            engine_.set_k_max(4.30);
            engine_.set_resonance(resonance);
            engine_.set_compensation(compensation);
            engine_.set_saturation_headroom(headroom);
            engine_.set_output_gain(1.0);
            engine_.set_cross_modulation(cross_mod, 2.0);
            engine_.set_drift(0.0, 0.0);
            engine_.set_pole_frequency(corner_hz_ / 0.4346);
            break;
        }

        case Voicing::minimoog: {
            constexpr double kFloorMakeup = 1.3396;
            const double makeup =
                linear_table(equivalent_knob, {kMinimoogMakeupKnots, kMinimoogMakeup});
            const double cutoff_headroom =
                linear_table(equivalent_knob, {kMinimoogHeadroomKnots, kMinimoogHeadroom});
            const double drop_db =
                linear_table(resonance_knob_, {kMinimoogLawKnots, kMinimoogDropDb});
            const double headroom_multiplier =
                linear_table(resonance_knob_, {kMinimoogLawKnots, kMinimoogHeadroomMultiplier});
            const double cross_mod =
                linear_table(resonance_knob_, {kMinimoogLawKnots, kMinimoogCrossMod});
            const double cutoff_cents =
                linear_table(resonance_knob_, {kMinimoogLawKnots, kMinimoogCutoffDriftCents});
            const double resonance_fraction =
                linear_table(resonance_knob_, {kMinimoogLawKnots, kMinimoogResonanceDriftFraction});
            const double drift_fade =
                corner_hz_ <= 900.0
                    ? 1.0
                    : (corner_hz_ >= 3000.0 ? 0.0 : (3000.0 - corner_hz_) / (3000.0 - 900.0));

            engine_.set_k_max(4.30);
            engine_.set_resonance(resonance);
            engine_.set_compensation(0.0);
            engine_.set_saturation_headroom(cutoff_headroom * headroom_multiplier / kFloorMakeup);
            engine_.set_output_gain(makeup * std::pow(10.0, drop_db / 20.0));
            engine_.set_cross_modulation(cross_mod, 2.0);
            engine_.set_drift(cutoff_cents * drift_fade, resonance_fraction * drift_fade, 1.5,
                              10.0);
            engine_.set_pole_frequency(corner_hz_ / 0.678);
            break;
        }
        }
    }

    inline static constexpr std::array<double, 13> kJunoCutoffKnots{
        0.0, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 1.0};
    inline static constexpr std::array<double, 13> kJunoCutoffHz{
        20.0, 45.0, 54.0, 72.0, 107.0, 166.0, 262.0, 424.0, 692.0, 1135.0, 1895.0, 3234.0, 18000.0};
    inline static constexpr std::array<double, 12> kJunoResonanceKnots{
        0.0, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 1.0};
    inline static constexpr std::array<double, 12> kJunoResonanceValues{
        0.0, 0.213, 0.314, 0.407, 0.490, 0.573, 0.653, 0.743, 0.775, 0.870, 0.93, 1.0};

    inline static constexpr std::array<double, 8> kJupiterCutoffKnots{0.0,  0.25, 0.35, 0.45,
                                                                      0.55, 0.65, 0.75, 1.0};
    inline static constexpr std::array<double, 8> kJupiterCutoffHz{20.0,  87.0,   157.0,  342.0,
                                                                   760.0, 1668.0, 3440.0, 18000.0};
    inline static constexpr std::array<double, 7> kJupiterResonanceKnots{0.0,  0.25, 0.50, 0.60,
                                                                         0.75, 0.90, 1.0};
    inline static constexpr std::array<double, 7> kJupiterResonanceValues{
        0.0, 0.349, 0.579, 0.622, 0.793, 0.921, 1.0};

    inline static constexpr std::array<double, 10> kProphetCutoffKnots{0.0,  0.25, 0.35, 0.45, 0.55,
                                                                       0.65, 0.75, 0.85, 0.95, 1.0};
    inline static constexpr std::array<double, 10> kProphetCutoffHz{
        4.0, 17.8, 35.6, 71.3, 142.6, 287.8, 573.0, 1151.2, 2291.0, 3172.0};
    inline static constexpr std::array<double, 7> kProphetResonanceKnots{0.0,  0.25, 0.50, 0.60,
                                                                         0.75, 0.90, 1.0};
    inline static constexpr std::array<double, 7> kProphetResonanceValues{
        0.0, 0.583, 0.7634, 0.963, 0.975, 0.99, 1.0};
    inline static constexpr std::array<double, 4> kProphetLawKnots{0.0, 0.60, 0.90, 1.0};
    inline static constexpr std::array<double, 4> kProphetCompensation{0.0, 0.0, 0.066, 0.083};
    inline static constexpr std::array<double, 4> kProphetHeadroom{1.0, 1.0, 1.90, 2.21};
    inline static constexpr std::array<double, 4> kProphetCrossMod{0.0, 0.0, 0.30, 0.33};

    inline static constexpr std::array<double, 9> kMinimoogCutoffKnots{0.0,  0.35, 0.45, 0.55, 0.65,
                                                                       0.75, 0.85, 0.95, 1.0};
    inline static constexpr std::array<double, 9> kMinimoogCutoffHz{
        20.0, 126.0, 347.0, 821.0, 1641.0, 3414.0, 7393.0, 12975.0, 17187.0};
    inline static constexpr std::array<double, 7> kMinimoogResonanceKnots{0.0,  0.25, 0.50, 0.60,
                                                                          0.75, 0.90, 1.0};
    inline static constexpr std::array<double, 7> kMinimoogResonanceValues{
        0.079, 0.079, 0.079, 0.52, 0.90, 0.97, 1.0};
    inline static constexpr std::array<double, 4> kMinimoogMakeupKnots{0.0, 0.10, 0.30, 1.0};
    inline static constexpr std::array<double, 4> kMinimoogMakeup{1.0, 1.0, 1.3396, 1.3396};
    inline static constexpr std::array<double, 7> kMinimoogHeadroomKnots{0.35, 0.55, 0.65, 0.75,
                                                                         0.85, 0.95, 1.0};
    inline static constexpr std::array<double, 7> kMinimoogHeadroom{1.0,  0.72,  1.163, 1.70,
                                                                    0.92, 0.752, 0.752};
    inline static constexpr std::array<double, 4> kMinimoogLawKnots{0.0, 0.60, 0.90, 1.0};
    inline static constexpr std::array<double, 4> kMinimoogDropDb{0.0, 0.0, -2.3, -3.07};
    inline static constexpr std::array<double, 4> kMinimoogHeadroomMultiplier{1.0, 1.0, 1.30,
                                                                              1.421};
    inline static constexpr std::array<double, 4> kMinimoogCrossMod{0.0, 0.0, 0.26, 0.29};
    inline static constexpr std::array<double, 4> kMinimoogCutoffDriftCents{0.0, 0.0, 12.0, 13.0};
    inline static constexpr std::array<double, 4> kMinimoogResonanceDriftFraction{0.0, 0.0, 0.0015,
                                                                                  0.0017};

    OtaCascadeFilterT<SampleType> engine_;
    Voicing voicing_ = Voicing::juno;
    double cutoff_knob_ = 0.5;
    double cutoff_modulation_octaves_ = 0.0;
    double resonance_knob_ = 0.0;
    double drive_db_ = 0.0;
    double corner_hz_ = 262.0;
};

/// Single-precision measured analog VCF.
using AnalogVcf = AnalogVcfT<float>;
/// Double-precision measured analog VCF.
using AnalogVcf64 = AnalogVcfT<double>;

}  // namespace pulp::signal
