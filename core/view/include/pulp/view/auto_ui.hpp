#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/state/store.hpp>
#include <memory>
#include <string>

namespace pulp::view {

// Automatically generates a UI from a StateStore's parameter definitions
// Creates knobs for continuous params, toggles for boolean params,
// with labels and grouping
class AutoUi {
public:
    // Build a view tree from the parameters in a StateStore
    static std::unique_ptr<View> build(state::StateStore& store);

    // Sync all auto-generated widget values from the store
    static void sync(View& root, state::StateStore& store);
};

} // namespace pulp::view
