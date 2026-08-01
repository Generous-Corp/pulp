#pragma once

/// @file snap_grid.hpp
/// Exact, meter-aware tick snapping for sequencer editing gestures.

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/quantize.hpp>

#include <cstdint>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// Which inclusive grid boundary a gesture requests.
enum class SnapDirection : std::uint8_t {
    AtOrBefore,
    Nearest,
    AtOrAfter,
};

enum class SnapGridError : std::uint8_t {
    InvalidInterval,
    InvalidSwingRatio,
};

/// A validated musical grid whose phase restarts at every authored bar.
///
/// The interval is expressed in ticks so callers can offer straight, dotted,
/// triplet or custom resolutions without a UI enum leaking into the kernel.
/// Every bar boundary is also a grid boundary, including when the interval
/// does not divide that bar. Swing warps interior boundaries in pairs and is
/// restarted from the bar, so a meter change cannot inherit the previous
/// signature's phase.
class SnapGrid {
  public:
    static runtime::Result<SnapGrid, SnapGridError>
    create(timebase::TickDuration interval,
           timebase::SwingRatio swing = timebase::kStraightSwing) noexcept;

    /// Snap `position` without leaving the signed tick domain.
    ///
    /// All directions include an exact boundary. Nearest resolves an exact
    /// midpoint toward the later tick so two input devices cannot disagree
    /// through incidental rounding.
    timebase::TickPosition snap(const timebase::CompiledMeterMap& meter_map,
                                timebase::TickPosition position,
                                SnapDirection direction = SnapDirection::Nearest) const noexcept;

    timebase::TickDuration interval() const noexcept {
        return interval_;
    }
    timebase::SwingRatio swing() const noexcept {
        return swing_;
    }

  private:
    SnapGrid(timebase::TickDuration interval, timebase::SwingRatio swing) noexcept
        : interval_(interval), swing_(swing) {}

    timebase::TickDuration interval_;
    timebase::SwingRatio swing_;
};

/// @}

} // namespace pulp::timeline_editor
