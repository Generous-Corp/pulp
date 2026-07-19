#pragma once

#include <pulp/view/design_frame_view.hpp>  // DesignFrameElement, ScalarSourceBinding

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

class View;
class HostParamSurface;

// Collaborator that owns the DesignFrameView sub-clusters concerned with
// host-parameter binding: per-element scalar sources, the bind grid, and the
// scale-mismatch diagnostic. Split out of DesignFrameView to keep that class
// focused on the frame/overlay/hit lifecycle; DesignFrameView holds one of these
// and forwards the relevant public methods to it. Behavior is unchanged — this
// is a pure extraction.
//
// Communication seam is narrow: the bind-grid and scale-mismatch methods take
// the view's `elements` and `HostParamSurface*` as explicit arguments per call,
// so the collaborator stores no back-pointer for them. The scalar-source cluster
// is the one exception: a ScalarSourceBinding must enrol with a View, so the
// collaborator holds `owner_` solely to construct ScalarSourceBinding(owner_).
class DesignParamBinding {
public:
    explicit DesignParamBinding(View& owner);

    // ── C: live per-element scalar sources ───────────────────────────────────
    // Bind (or, with a null source, unbind) the ScalarSource for `param_key`, then
    // re-point the active frame's slot table against `elements`. See
    // DesignFrameView::set_element_scalar_source for the public contract.
    void set_scalar_source(const std::string& param_key,
                           std::shared_ptr<ScalarSource> source,
                           const std::vector<DesignFrameElement>& elements);

    // Latest value published for element `i`, or 0 when out of range / unbound.
    float element_scalar(int i) const;

    // Whether element `i` of the active frame has a scalar source bound.
    bool element_has_scalar_source(int i) const;

    // Re-point the active frame's slot table at the bindings whose param_key
    // `elements` declares, and park the rest. Called after every elements_ swap.
    void rebuild_slots(const std::vector<DesignFrameElement>& elements);

    // ── B: bind grid ─────────────────────────────────────────────────────────
    // Replace the grid's key set, dropping the previous grid's stand-ins off the
    // tail of `elements`, then re-apply against `elements`. See
    // DesignFrameView::build_bind_grid for the public contract.
    void build_grid(std::vector<std::string> keys,
                    std::vector<DesignFrameElement>& elements);

    // Append the bind grid's stand-in elements to `elements` for keys the active
    // frame carries no control for. Called after every frame activation.
    void apply_grid(std::vector<DesignFrameElement>& elements);

    // Whether element `i` is a bind-grid stand-in rather than a design control.
    // `element_count` is the active frame's live element count (upper bound).
    bool is_stand_in(int i, int element_count) const;

    // The keys the bind grid was last built with.
    const std::vector<std::string>& grid_keys() const { return bind_grid_keys_; }

    // ── A: param-scale mismatch diagnostic ───────────────────────────────────
    // Record a distinct param-scale mismatch and fire the diagnostic callback.
    // De-duplicates by param_key. Callable from DesignFrameView's const normalize
    // path (resolve_value_count) because it is reached through the collaborator
    // pointer, whose const-ness the const view does not propagate — the
    // accumulator is a diagnostic log, not observable view state.
    void report_mismatch(const std::string& key, int ui_count, int host_count,
                         bool host_has_param);

    // The host's value cardinality for `key`, or 0 when `hp` does not resolve it.
    int host_step_count_for(const std::string& key, HostParamSurface* hp) const;

    // Whether the live surface `hp` carries `key` — ParamScaleMismatch::host_has_param.
    bool host_has_param_for(const std::string& key, HostParamSurface* hp) const;

    // Install the diagnostic callback and replay the mismatches already seen. See
    // DesignFrameView::set_on_param_scale_mismatch for the public contract.
    void set_on_mismatch(std::function<void(const ParamScaleMismatch&)> cb);

    // The distinct param-scale mismatches seen so far (first-seen order, deduped).
    const std::vector<ParamScaleMismatch>& mismatches() const {
        return param_scale_mismatches_;
    }

private:
    View& owner_;

    // Element scalar bindings, owned by stable param_key so they survive a frame
    // swap, plus the active frame's index-aligned view of them (non-owning; null
    // where the element has no binding) so element_scalar() stays paint-safe.
    std::unordered_map<std::string, std::unique_ptr<ScalarSourceBinding>> element_scalars_;
    std::vector<ScalarSourceBinding*> active_element_scalars_;

    // Bind grid: the caller-supplied host-parameter keys, and the index of the
    // first stand-in in the active frame's elements (all stand-ins are appended
    // after the frame's own elements, so one index bounds them). -1 = no grid.
    std::vector<std::string> bind_grid_keys_;
    int bind_grid_begin_ = -1;

    // Param-scale mismatch diagnostic accumulator + callback. Written by
    // report_mismatch, which the const normalize path reaches through this
    // collaborator; the accumulator records what was observed and never feeds
    // back into what the view emits or renders.
    std::function<void(const ParamScaleMismatch&)> on_param_scale_mismatch_;
    std::vector<ParamScaleMismatch> param_scale_mismatches_;
};

}  // namespace pulp::view
