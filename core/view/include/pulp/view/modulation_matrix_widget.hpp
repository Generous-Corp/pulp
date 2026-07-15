#pragma once

// Modulation-matrix canvas widget.
//
// Thin View that visualises a ModulationMatrix: draws source nodes on
// the left, destinations on the right, and one line per route with a
// thickness/color proportional to depth. Clicking a source then a
// destination adds a route. Clicking an existing route's line removes
// it.
//
// The widget owns visualisation + hit-testing. It adds routes at
// depth = 1.0 by default; selected route depth and curve are mutable
// through the public setters so plugins can wire their own external
// controls.

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

    /// Currently-selected route index (the route the user is editing).
    /// -1 when none. Paired with set_selected_route_depth / curve to
    /// drive external depth-slider and curve-dropdown widgets.
    int selected_route() const { return selected_route_; }

    /// Set the selected route by index (into matrix_->routes()). -1 clears.
    void set_selected_route(int idx) { selected_route_ = idx; }

    /// Mutate the depth of the selected route. No-op if no route is
    /// selected. depth is clamped to [-1, 1]; sign is the bipolar flag.
    /// Returns the new depth (or 0 if no-op).
    float set_selected_route_depth(float depth);

    /// Mutate the curve of the selected route. No-op if no route is selected.
    void set_selected_route_curve(ModCurve curve);

    /// Remove the selected route. No-op if no route is selected.
    /// After remove, selected_route() returns -1.
    void remove_selected_route();

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_cancel(Point pos) override { (void)pos; pending_source_ = -1; }
    bool wants_mouse_input() const override { return true; }

private:
    ModulationMatrix* matrix_ = nullptr;
    std::vector<Endpoint> sources_;
    std::vector<Endpoint> destinations_;
    int pending_source_ = -1;   ///< index into sources_
    int selected_route_ = -1;   ///< index into matrix_->routes()

    // Layout helpers
    float row_height_() const;
    float source_column_x_() const;
    float dest_column_x_()   const;

    /// Hit-test a point against existing route lines. Returns the route
    /// index (into matrix_->routes()) under the cursor or -1 if none.
    int  route_at_(Point pos) const;
};

}  // namespace pulp::view
