#pragma once

#include <pulp/timebase/tick.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace pulp::timeline::detail {

// Conversion between an SMF header division (ticks per quarter note) and the
// canonical timebase grid. Both grids are quarter-note anchored, so scaling is
// a pure ratio; both directions round half away from zero on a non-representable
// input and report the resulting error rather than hiding it.
struct TickScale {
    std::int64_t division = 1;
    // kTicksPerQuarter % division == 0: every SMF tick has an exact canonical
    // representation, so importing that division never rounds.
    bool exact = true;

    static TickScale create(std::int64_t division) noexcept {
        // Guard the modulo. Callers construct a TickScale in their member-init
        // list and only validate the division inside run(), so an out-of-range
        // division reaches this before anyone has rejected it — and `% 0` is
        // undefined behaviour, not a value. On arm64 it neither traps nor
        // reports, so the only signal is a sanitizer. A zero division is never
        // exact by definition, which is what the validation downstream then
        // rejects as UnsupportedDivision.
        return TickScale{division, division != 0 && timebase::kTicksPerQuarter % division == 0};
    }
};

// Scale an SMF tick up to the canonical grid. Returns std::nullopt when the
// exact product would leave the signed 64-bit domain.
inline std::optional<std::int64_t> smf_to_canonical_ticks(std::int64_t smf_tick,
                                                          const TickScale& scale) noexcept {
    if (smf_tick < 0)
        return std::nullopt;
    const auto half = scale.division / 2;
    if (smf_tick > (std::numeric_limits<std::int64_t>::max() - half) / timebase::kTicksPerQuarter)
        return std::nullopt;
    return (smf_tick * timebase::kTicksPerQuarter + half) / scale.division;
}

struct SmfTickConversion {
    std::int64_t smf_tick = 0;
    // True when the division represents the canonical tick with no rounding,
    // i.e. canonical * division is a whole number of canonical quarter notes.
    bool exact = true;
    // |canonical - smf_to_canonical_ticks(smf_tick)|, in canonical ticks.
    std::int64_t rounding_error = 0;
};

// Scale a canonical tick down to the SMF grid, reporting whether the grid can
// hold it and how far the round trip lands from the input.
inline std::optional<SmfTickConversion>
canonical_to_smf_ticks(std::int64_t canonical_tick, const TickScale& scale) noexcept {
    constexpr auto kQuarter = timebase::kTicksPerQuarter;
    if (canonical_tick < 0)
        return std::nullopt;
    if (canonical_tick > (std::numeric_limits<std::int64_t>::max() - kQuarter / 2) / scale.division)
        return std::nullopt;
    const auto product = canonical_tick * scale.division;
    const auto smf_tick = (product + kQuarter / 2) / kQuarter;
    const auto reconstructed = smf_to_canonical_ticks(smf_tick, scale);
    if (!reconstructed)
        return std::nullopt;
    const auto difference = canonical_tick - *reconstructed;
    return SmfTickConversion{smf_tick, product % kQuarter == 0,
                             difference < 0 ? -difference : difference};
}

} // namespace pulp::timeline::detail
