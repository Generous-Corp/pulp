#include <pulp/view/design_param_binding.hpp>

#include <pulp/view/host_param_surface.hpp>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace pulp::view {

namespace {
// Where a bind-grid stand-in parks (SVG coordinates). Far outside any plausible
// panel, so a stand-in carries no on-screen geometry even if a future painter
// grows a fallback for a needle-less element. Not a hit-test guard on its own —
// a stand-in is also disabled and has a zero hit radius; this is belt-and-braces
// on the geometry axis.
constexpr float kBindGridOffscreen = -1.0e6f;
}  // namespace

DesignParamBinding::DesignParamBinding(View& owner) : owner_(owner) {}

// ── C: live per-element scalar sources ──────────────────────────────────────

void DesignParamBinding::set_scalar_source(const std::string& param_key,
                                           std::shared_ptr<ScalarSource> source,
                                           const std::vector<DesignFrameElement>& elements) {
    if (param_key.empty()) return;  // never a valid element identity
    if (!source) {
        // Unbinding drops the binding (and its subscription), so an unbound view
        // stops holding the editor's frames alive.
        element_scalars_.erase(param_key);
        rebuild_slots(elements);
        return;
    }
    auto& slot = element_scalars_[param_key];
    if (!slot) slot = std::make_unique<ScalarSourceBinding>(owner_);
    slot->set_source(std::move(source));
    rebuild_slots(elements);
}

float DesignParamBinding::element_scalar(int i) const {
    if (i < 0 || i >= static_cast<int>(active_element_scalars_.size())) return 0.0f;
    const ScalarSourceBinding* b = active_element_scalars_[i];
    return b ? b->value() : 0.0f;
}

bool DesignParamBinding::element_has_scalar_source(int i) const {
    if (i < 0 || i >= static_cast<int>(active_element_scalars_.size())) return false;
    return active_element_scalars_[i] != nullptr;
}

void DesignParamBinding::rebuild_slots(const std::vector<DesignFrameElement>& elements) {
    active_element_scalars_.assign(elements.size(), nullptr);
    if (element_scalars_.empty()) return;

    for (size_t i = 0; i < elements.size(); ++i) {
        const std::string& key = elements[i].param_key;
        if (key.empty()) continue;
        auto it = element_scalars_.find(key);
        if (it == element_scalars_.end() || !it->second) continue;
        active_element_scalars_[i] = it->second.get();
    }

    // A binding is live iff the ACTIVE frame declares its key. A key no frame
    // carries — a typo, or a param dropped from a redesign — would otherwise
    // hold the editor at full frame rate forever to feed a ring nothing paints,
    // silently costing the plugin the idle-at-0-fps behavior the whole unbind
    // path exists to protect. Decided from the rebuilt table rather than by
    // parking everything and un-parking the matches, so a binding that stays
    // matched is never toggled off and on — that would drop its cached value and
    // churn its subscription on every rebuild, including the rebuild that runs
    // when a SIBLING element is bound.
    for (auto& [key, binding] : element_scalars_) {
        if (!binding) continue;
        const bool declared = std::find(active_element_scalars_.begin(),
                                        active_element_scalars_.end(),
                                        binding.get()) != active_element_scalars_.end();
        binding->set_active(declared);
    }
}

// ── B: bind grid ────────────────────────────────────────────────────────────

void DesignParamBinding::build_grid(std::vector<std::string> keys,
                                    std::vector<DesignFrameElement>& elements) {
    // Drop the previous grid before rebuilding: repeated calls replace, never
    // accumulate. bind_grid_begin_ names the tail of elements that apply_grid
    // appended, and it is re-established on every frame activation, so erasing
    // that tail can only ever remove stand-ins. Rebuilding the overlays is not
    // needed: a stand-in never builds one (a needle-less knob is skipped), so the
    // overlay->element indices of the real controls ahead of the tail are stable.
    if (bind_grid_begin_ >= 0 && bind_grid_begin_ <= static_cast<int>(elements.size()))
        elements.erase(elements.begin() + bind_grid_begin_, elements.end());
    bind_grid_begin_ = -1;
    bind_grid_keys_ = std::move(keys);
    apply_grid(elements);
}

void DesignParamBinding::apply_grid(std::vector<DesignFrameElement>& elements) {
    if (bind_grid_keys_.empty()) {
        bind_grid_begin_ = -1;
        return;
    }
    bind_grid_begin_ = static_cast<int>(elements.size());
    // The keys already spoken for, gathered once. element_for_param_key is a
    // linear scan, so asking it per key would be O(keys x elements) — ~35k string
    // compares for a 188-parameter plug-in, on every frame swap. A stand-in's own
    // key joins the set as it is created, so a duplicate key in bind_grid_keys_
    // still produces exactly one stand-in, as the per-key scan did.
    std::unordered_set<std::string> bound;
    bound.reserve(elements.size() + bind_grid_keys_.size());
    for (const auto& e : elements)
        if (!e.param_key.empty()) bound.insert(e.param_key);

    for (const std::string& key : bind_grid_keys_) {
        if (key.empty()) continue;
        // A design's own control for this key always wins: it is earlier in
        // elements, so element_for_param_key finds it first. Skipping keeps the
        // grid to genuine gaps rather than shadowing a real control with a
        // stand-in that can never be reached.
        if (!bound.insert(key).second) continue;
        DesignFrameElement e;
        // A knob with no needle path is skipped by paint() and builds no overlay
        // widget, so a stand-in renders nothing at all.
        e.kind = DesignFrameElement::Kind::knob;
        e.needle_d.clear();
        e.hit_radius = 0.0f;   // knob hit-testing needs distance < radius: never true
        e.enabled = false;     // and hit_element skips a disabled element outright
        // Parked outside the panel so the element carries no on-screen geometry
        // even if a future painter grows a fallback for a needle-less element.
        e.cx = e.cy = e.x = e.y = kBindGridOffscreen;
        e.w = e.h = 0.0f;
        e.param_key = key;
        elements.push_back(std::move(e));
    }
}

bool DesignParamBinding::is_stand_in(int i, int element_count) const {
    return bind_grid_begin_ >= 0 && i >= bind_grid_begin_ && i < element_count;
}

// ── A: param-scale mismatch diagnostic ──────────────────────────────────────

void DesignParamBinding::report_mismatch(const std::string& key, int ui_count,
                                         int host_count, bool host_has_param) {
    if (key.empty()) return;
    const bool seen = std::any_of(param_scale_mismatches_.begin(),
                                  param_scale_mismatches_.end(),
                                  [&](const ParamScaleMismatch& m) {
                                      return m.param_key == key;
                                  });
    if (seen) return;
    ParamScaleMismatch m{key, ui_count, host_count, host_has_param};
    param_scale_mismatches_.push_back(m);
    if (on_param_scale_mismatch_) on_param_scale_mismatch_(m);
}

// Whether a live surface carries `key` at all — the field that splits "the host
// answered 'no index domain'" from "the host has never heard of this key".
bool DesignParamBinding::host_has_param_for(const std::string& key,
                                            HostParamSurface* hp) const {
    return hp && !key.empty() && hp->has_param(key);
}

// The host's own cardinality for `key`, or 0 when no surface resolves it.
// Used to report an unbound key honestly (the host's count, not a placeholder).
int DesignParamBinding::host_step_count_for(const std::string& key,
                                            HostParamSurface* hp) const {
    if (!host_has_param_for(key, hp)) return 0;
    return hp->param_step_count(key);
}

void DesignParamBinding::set_on_mismatch(
    std::function<void(const ParamScaleMismatch&)> cb) {
    on_param_scale_mismatch_ = std::move(cb);
    // Replay what was already observed, so a callback attached after the first
    // tick still learns about it (matches set_on_unregistered_custom_control).
    if (on_param_scale_mismatch_)
        for (const auto& m : param_scale_mismatches_) on_param_scale_mismatch_(m);
}

}  // namespace pulp::view
