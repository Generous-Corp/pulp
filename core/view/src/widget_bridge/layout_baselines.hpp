#pragma once

// The layout style each widget had before the bridge first wrote to it.
//
// `resetStyleProperty` hands a slot back to this value when a style key
// disappears from a React commit. A widget's starting layout is not
// `FlexStyle{}`: `createRow` starts as a row, a materialized design node
// starts with the geometry the importer gave it. Recording the style on the
// first layout write captures exactly that, without each factory having to
// declare its defaults.
//
// Entries are keyed by `View::import_binding_instance_id()`, which is unique
// per View instance for the life of the process, so a recycled allocation or
// a widget re-created under the same id never inherits a stale baseline.
// `forget_layout_baselines` prunes entries when the bridge forgets a subtree;
// entries for views destroyed any other way are swept once they expire.

#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::view {

struct LayoutBaseline {
    FlexStyle flex;
    GridStyle grid;
};

/// Record `view`'s current flex and grid style unless a baseline already
/// exists for it. Call before the first mutation of either.
void capture_layout_baseline(const View& view);

/// Replace `view`'s baseline. For a view that takes over another's layout
/// role, whose starting style is the other view's rather than its own.
void set_layout_baseline(const View& view, LayoutBaseline baseline);

/// The baseline recorded for `view`, or nullopt when no layout write has
/// reached it yet (its current style is then still its starting style).
std::optional<LayoutBaseline> find_layout_baseline(const View& view);

/// Drop the baselines of views that are leaving the bridge.
void forget_layout_baselines(const std::vector<std::uint64_t>& instance_ids);

/// Number of recorded baselines, for tests that check pruning.
std::size_t layout_baseline_count();

} // namespace pulp::view
