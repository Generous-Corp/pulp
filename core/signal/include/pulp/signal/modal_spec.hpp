#pragma once

/// @file modal_spec.hpp
/// ModalSpec — a modal instrument as a data file rather than a C++ class.
///
/// A ModalSpec is the complete description of a struck/plucked resonator:
/// its modes, how it is excited, how strike and pickup position weight each
/// mode, and — the part that makes it a *contract* rather than a blob — the
/// tolerances within which a render of it must reproduce those modes. The
/// spec is its own acceptance criterion: a harness can load a spec, render
/// it, measure the render, and decide PASS/FAIL without a human in the loop
/// and without a golden file to keep in sync.
///
/// The spec drives `ModalBankT` and does not modify it. Everything it can
/// express is a mode set the bank already plays.
///
/// ── The gain contract ─────────────────────────────────────────────────────
/// `ModalBankT` scales its excitation by `gain` such that a mode's impulse
/// response is exactly `gain * r^n * sin((n+1) w)`. So the `gain` written in a
/// spec is the amplitude a measurement of the rendered impulse response reads
/// back: the number you author and the number the harness measures are the
/// same number. `resolve_modes()` preserves this exactly when the spec
/// carries no shape maps; when it does, the resolved gain is
/// `gain * strike_weight * pickup_weight` and that product is what the
/// measurement reads back. Any change here must keep that property — it is
/// what allows `tolerances.gain_rel` to be checked against a render at all.
///
/// ── Layering ──────────────────────────────────────────────────────────────
/// This header is pure data + arithmetic: it depends on `modal_bank.hpp` and
/// the standard library, and knows nothing about JSON. The serialization
/// functions declared at the bottom are implemented in `modal_spec.cpp`,
/// which is the only place choc's JSON parser is included. That firewall
/// keeps `pulp-signal` — a header-only INTERFACE target that every DSP
/// translation unit pulls in — free of a JSON dependency.
///
/// ── Threading ─────────────────────────────────────────────────────────────
/// Parsing, serializing and validating are loader-thread operations and
/// allocate freely. `resolve_modes()` and `load_modes()` allocate only when
/// the caller's scratch vector needs to grow, so a caller that pre-sizes its
/// scratch can re-resolve (to move a strike position, say) without
/// allocating. Nothing here is safe to call from `process()` regardless;
/// `ModalBankT::set_modes()` evaluates transcendentals per mode.

#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::signal {

/// Schema version this build writes and is the highest it can read.
///
/// Bumped only for a *breaking* change — a field whose meaning changed, or
/// one whose absence can no longer be defaulted safely. Additive fields ship
/// without a bump and are picked up by the unknown-field policy below, so an
/// older build keeps loading a newer additive file instead of rejecting it.
inline constexpr int kModalSpecSchemaVersion = 1;

/// How the resonator is struck. The contact duration is the excitation's
/// only spectral control: a short contact has a flat spectrum that reaches
/// the high modes, a long one rolls them off (see `fill_strike_pulse`).
struct ModalSpecExcitation {
    double contact_s = 0.001;  ///< contact duration; 0 means a unit impulse
    double velocity = 1.0;     ///< peak of the contact pulse
};

/// A mode shape sampled on a uniform grid across the object's normalized
/// extent [0, 1], mode-major.
///
/// Physically this is phi_m(x): how strongly position x couples to mode m.
/// It is sampled rather than analytic because a spec has to be able to carry
/// a measured shape as easily as an ideal one. An empty map means position
/// has no effect and every mode couples with weight 1 — which is also the
/// state that preserves authored gains verbatim.
struct ModalShapeMap {
    /// Samples per mode across [0, 1]. Zero means "no map".
    int grid_points = 0;
    /// `grid_points` weights per mode, laid out `[mode * grid_points + g]`.
    std::vector<float> weights;

    bool empty() const { return grid_points <= 0 || weights.empty(); }

    /// Linearly interpolated weight of `mode` at normalized `position`.
    /// `position` is clamped to [0, 1]; an empty map returns 1.
    float weight_at(int mode, float position) const {
        if (empty()) return 1.0f;
        const int last = grid_points - 1;
        const std::size_t base = static_cast<std::size_t>(mode) *
                                 static_cast<std::size_t>(grid_points);
        if (base + static_cast<std::size_t>(grid_points) > weights.size()) return 1.0f;
        if (last == 0) return weights[base];
        const float x = std::clamp(position, 0.0f, 1.0f) * static_cast<float>(last);
        const int i0 = std::min(static_cast<int>(x), last - 1);
        const float frac = x - static_cast<float>(i0);
        const float w0 = weights[base + static_cast<std::size_t>(i0)];
        const float w1 = weights[base + static_cast<std::size_t>(i0) + 1];
        return w0 + (w1 - w0) * frac;
    }
};

/// The PASS criteria a render of this spec must meet. These are part of the
/// spec — not of the test that reads it — because the tolerance a model can
/// hold is a property of the model: a bar with well-separated modes holds a
/// few cents, a dense plate does not, and burying that number in test code
/// makes it invisible to whoever authors the next spec.
///
/// **These are defined against the spec's impulse response** — the resolved
/// modes driven by a single unit sample — not against a render through
/// `excitation`. A contact pulse shapes the excitation spectrum by design, so
/// it scales each mode's measured amplitude by an amount that is a property
/// of the pulse rather than of the mode set. The impulse response is the mode
/// set's own definition, and is the only stimulus under which the authored
/// `gain` and the measured amplitude are the same number. `excitation`
/// describes performance; these tolerances describe identity.
struct ModalSpecTolerances {
    /// Frequency error budget per mode, in cents.
    double freq_cents = 10.0;
    /// T60 error budget per mode, relative (0.08 = 8%).
    double t60_rel = 0.08;
    /// Amplitude error budget per mode, relative to the resolved gain.
    double gain_rel = 0.10;
    /// Impulse response length a verifier should render to check the above.
    /// Too short and a long mode's decay cannot be fit at all, so the spec
    /// states the length its own tolerances were set against.
    double verify_seconds = 2.0;
};

/// A modal instrument, complete.
struct ModalSpec {
    int schema_version = kModalSpecSchemaVersion;
    std::string name;
    std::string description;
    std::vector<ModalMode> modes;
    ModalSpecExcitation excitation;
    /// Optional. When present, must hold `grid_points` weights per mode.
    ModalShapeMap strike_map;
    ModalShapeMap pickup_map;
    ModalSpecTolerances tolerances;

    /// Contact duration in samples at `sample_rate`, for `fill_strike_pulse`.
    /// Returns 0 for an impulse excitation, which callers render as a single
    /// unit sample rather than a pulse.
    int contact_samples(double sample_rate) const {
        const double n = excitation.contact_s * sample_rate;
        return n <= 0.0 ? 0 : static_cast<int>(n + 0.5);
    }
};

/// Resolve the spec's modes at a strike/pickup position into `out`.
///
/// With no shape maps this copies `modes` verbatim, gains included — the
/// property the whole verification story rests on. With maps, each gain is
/// multiplied by its strike and pickup weights. `out` is resized rather than
/// reallocated when it already has capacity, so a caller that pre-sizes it
/// can sweep a strike position without allocating.
inline void resolve_modes(const ModalSpec& spec, float strike_position,
                          float pickup_position, std::vector<ModalMode>& out) {
    out.resize(spec.modes.size());
    for (std::size_t m = 0; m < spec.modes.size(); ++m) {
        ModalMode mode = spec.modes[m];
        const int mi = static_cast<int>(m);
        mode.gain *= spec.strike_map.weight_at(mi, strike_position) *
                     spec.pickup_map.weight_at(mi, pickup_position);
        out[m] = mode;
    }
}

/// Convenience overload; allocates.
inline std::vector<ModalMode> resolve_modes(const ModalSpec& spec,
                                            float strike_position = 0.5f,
                                            float pickup_position = 0.5f) {
    std::vector<ModalMode> out;
    resolve_modes(spec, strike_position, pickup_position, out);
    return out;
}

/// Resolve the spec at a strike/pickup position and load the result into an
/// already-prepared `bank`. `scratch` holds the resolved modes; reuse the
/// same vector across calls to keep a live position sweep allocation-free.
/// The bank must have been prepared for at least `spec.modes.size()` modes —
/// `ModalBankT::set_modes` silently ignores the excess otherwise.
template <typename SampleType>
void load_modes(const ModalSpec& spec, ModalBankT<SampleType>& bank,
                std::vector<ModalMode>& scratch, float strike_position = 0.5f,
                float pickup_position = 0.5f) {
    resolve_modes(spec, strike_position, pickup_position, scratch);
    bank.set_modes(scratch);
}

/// Prepare `bank` to hold this spec's modes at `sample_rate` and load them.
/// Allocates (prepare()); loader thread only.
template <typename SampleType>
void prepare_and_load(const ModalSpec& spec, ModalBankT<SampleType>& bank,
                      double sample_rate, float strike_position = 0.5f,
                      float pickup_position = 0.5f) {
    bank.prepare(sample_rate, static_cast<int>(spec.modes.size()));
    std::vector<ModalMode> scratch;
    load_modes(spec, bank, scratch, strike_position, pickup_position);
}

/// Check a spec's internal consistency. Returns false and fills `error` with
/// a message naming the offending field (and mode index, where there is one)
/// on the first problem found. `parse_modal_spec` runs this, so a spec that
/// parses is already valid; call it directly after building a spec in code.
bool validate_modal_spec(const ModalSpec& spec, std::string& error);

/// What a parse found beyond the spec itself. Always populated; read it.
struct ModalSpecDiagnostics {
    /// Empty iff the parse succeeded. Names the field and, for JSON syntax
    /// errors, the line and column.
    std::string error;
    /// Dotted paths of fields that were present, unrecognized, and ignored
    /// (see the unknown-field policy on `parse_modal_spec`). A tool that
    /// round-trips specs should surface these — they will not survive.
    std::vector<std::string> unknown_fields;
};

/// Parse a ModalSpec from JSON.
///
/// Returns `std::nullopt` on any failure — malformed JSON, a missing or
/// mistyped required field, a value that fails validation — with a message
/// in `diagnostics->error`. It never returns a partially-populated spec and
/// never silently substitutes a default for a field that was present but
/// wrong: a typo'd t60 must fail loudly, because a spec whose numbers are not
/// the numbers you wrote is worse than no spec.
///
/// ── Version and unknown-field policy ──────────────────────────────────────
///   * `schema_version` is required. A version **newer** than
///     `kModalSpecSchemaVersion` is **rejected** — a breaking change is the
///     only reason the version moves, so this build provably cannot read it.
///     Older versions are accepted (v1 is the only version so far).
///   * Unrecognized fields at any level are **ignored** and reported in
///     `diagnostics->unknown_fields`. This is what lets an additive field
///     ship without a version bump.
///   * Ignored fields are **not preserved**: `to_json(parse(x))` drops them.
///     Loading and re-saving a file written by a newer producer therefore
///     loses that producer's additions. Tools that re-save should check
///     `unknown_fields` and refuse, or warn, rather than quietly truncate.
std::optional<ModalSpec> parse_modal_spec(std::string_view json,
                                          ModalSpecDiagnostics* diagnostics = nullptr);

/// Serialize a spec to JSON. Emits every known field, including defaulted
/// ones, so the output is a complete self-describing document and a diff
/// between two specs shows every difference rather than only the overridden
/// ones. `pretty` emits line breaks — leave it on for files people read and
/// version-control.
///
/// Round-trip contract: for any spec that `parse_modal_spec` accepted,
/// `parse_modal_spec(to_json(s))` yields a spec equal to `s` (modulo any
/// unknown fields, which are already gone by then).
std::string to_json(const ModalSpec& spec, bool pretty = true);

/// Exact equality over every known field. Float fields are compared bit-wise;
/// this is a round-trip check, not a perceptual one.
bool operator==(const ModalSpec& a, const ModalSpec& b);
inline bool operator!=(const ModalSpec& a, const ModalSpec& b) { return !(a == b); }

} // namespace pulp::signal
