#pragma once

// The run segment: "when did this run start?"
//
// The clock grid is a pure function of the host's position, which is exactly why
// it cannot express the features users actually reach for. "Delay the first
// pulse", "skip the first pulse", "wait for the bar line", "reset every N bars"
// are all defined relative to *when the user hit play*, not to the absolute
// timeline. Two runs starting at different beats must behave the same way.
//
// So the suite keeps one explicit origin, captured on the transport's play edge
// (`ProcessContext::transport_started`), and hangs every run-relative feature off
// it. This is the whole of the plug-in's musical state. Keeping it in one named
// place is what stops it from being smeared across half a dozen accumulators —
// which is how "position-derived clock" quietly decays back into the burst bug it
// was chosen to prevent.

#include <cmath>

namespace pulp::examples::brew {

/// Beats in one bar, from the host's time signature. Denominator 4 means the beat
/// unit *is* Pulp's quarter-note position unit, so 6/8 is 3 quarter notes.
[[nodiscard]] inline double beats_per_bar(int numerator, int denominator) noexcept {
    if (numerator < 1 || denominator < 1) return 4.0;
    return 4.0 * static_cast<double>(numerator) / static_cast<double>(denominator);
}

/// The first bar line at or after `beats`.
///
/// Derived from the beat count rather than from `ProcessContext::bar`, whose
/// origin is not consistent across formats. The assumption is that bar 1 begins
/// at beat 0 — true in every host we test, and wrong for a project with a pickup
/// bar, where the wait would land one bar line early.
[[nodiscard]] inline double next_bar_at_or_after(double beats,
                                                 double bar_beats) noexcept {
    if (!(bar_beats > 0.0)) return beats;
    const double bars = beats / bar_beats;
    const double rounded = std::round(bars);
    // Already on a bar line (within a hair): that line is the answer, not the next.
    if (std::abs(bars - rounded) < 1e-9) return rounded * bar_beats;
    return std::ceil(bars) * bar_beats;
}

/// Per-run state. Everything here is reset by `begin()` on the play edge.
struct RunSegment {
    /// Beat at which the transport started running.
    double origin_beats = 0.0;
    /// Edges strictly before this beat are suppressed (the 1st-delay / wait-bar gate).
    double gate_beats = 0.0;
    /// Whether the "skip first pulse" allowance has been spent this run.
    bool skip_consumed = false;

    void begin(double position_beats, double gate) noexcept {
        origin_beats = position_beats;
        gate_beats = gate;
        skip_consumed = false;
    }

    /// Is an edge at `edge_beats` past the gate?
    [[nodiscard]] bool passes_gate(double edge_beats) const noexcept {
        return edge_beats >= gate_beats - 1e-9;
    }
};

}  // namespace pulp::examples::brew
