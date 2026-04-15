#pragma once

// Modulation-matrix canvas widget — workstream 07 slice B5.
//
// Thin View that visualises a ModulationMatrix: draws source nodes on
// the left, destinations on the right, and one line per route with a
// thickness/colour proportional to depth. Clicking a source then a
// destination adds a route. Clicking an existing route's line removes
// it.
//
// This slice lands the visualisation + hit-testing scaffolding. Per-
// route depth sliders and curve dropdowns are a follow-up; for now the
// widget adds routes at depth = 1.0 and the depth is edited via any
// other UI the plug-in exposes.

#include <pulp/view/modulation_matrix.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

namespace pulp::view {

class ModulationMatrixWidget : public View {
public:
    /// Populate the source + destination labels. Each label is
    /// paired with the underlying ModSourceId / ModDestinationId so
    /// clicks translate back into matrix ops.
    struct Endpoint {
        std::string label;
        uint32_t    id = 0;
    };

    void set_matrix(ModulationMatrix* matrix) { matrix_ = matrix; invalidate_layout(); }
    void set_sources(std::vector<Endpoint> sources)      { sources_      = std::move(sources); invalidate_layout(); }
    void set_destinations(std::vector<Endpoint> dests)   { destinations_ = std::move(dests);   invalidate_layout(); }

    ModulationMatrix* matrix()                      const { return matrix_; }
    const std::vector<Endpoint>& sources()          const { return sources_; }
    const std::vector<Endpoint>& destinations()     const { return destinations_; }

    /// Currently-selected source (for the click-source-then-destination
    /// gesture). -1 when no source is pending.
    int pending_source() const { return pending_source_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    ModulationMatrix* matrix_ = nullptr;
    std::vector<Endpoint> sources_;
    std::vector<Endpoint> destinations_;
    int pending_source_ = -1;  ///< index into sources_

    // Layout helpers
    float row_height_() const;
    float source_column_x_() const;
    float dest_column_x_()   const;
};

}  // namespace pulp::view
