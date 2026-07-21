#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/state/store.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace pulp::view {

// Automatically generates a UI from a StateStore's parameter definitions
// Creates knobs for continuous params, toggles for boolean params,
// with labels and grouping
class AutoUi {
public:
    /// Logical-pixel size that fits AutoUi's generated grid without clipping.
    struct SizeHint {
        std::uint32_t width;
        std::uint32_t height;
    };

    // Build a view tree from the parameters in a StateStore
    static std::unique_ptr<View> build(state::StateStore& store);

    /// Compute a design size that fits every parameter tile AutoUi would
    /// generate for @p store — the grid wrapped to a sensible column count,
    /// plus the "Parameters" title, padding, and (when present) group boxes.
    ///
    /// The format adapters adopt this as the editor's preferred size (via
    /// `view_size_from_design()`) when the processor declares no explicit
    /// size, so the default editor OPENS large enough to show all knobs and
    /// resizes proportionally instead of clipping its top row. Kept in AutoUi
    /// so the fit math and `build()`'s layout share the same tile constants.
    static SizeHint preferred_size(state::StateStore& store);

    // Sync all auto-generated widget values from the store
    static void sync(View& root, state::StateStore& store);
};

} // namespace pulp::view
