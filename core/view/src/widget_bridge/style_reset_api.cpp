// widget_bridge/style_reset_api.cpp - resetStyleProperty and the per-widget
// layout baselines it restores from.
//
// A style key that disappears from a React commit has to hand its slot back
// to the value the widget started with. Most paint slots have a setter whose
// argument already means "unset" (an empty background, a cleared shadow),
// and @pulp/react resets those itself. Layout slots do not: per-edge padding
// and margin, flex-basis and grid placement use sentinels and typed
// dimensions no setter argument can express, and a widget's starting layout
// differs by factory. resetStyleProperty restores those slots natively.

#include "api_registry.hpp"
#include "layout_baselines.hpp"
#include <pulp/view/widget_bridge.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulp::view {

namespace {

std::mutex& baseline_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct StoredBaseline {
    LayoutBaseline baseline;
    // Expires with the View (or when a pooled View rotates its identity), so
    // entries for views that died without passing through
    // forget_layout_baselines can still be swept.
    std::weak_ptr<const std::uint64_t> lifetime;
};

struct BaselineStore {
    std::unordered_map<std::uint64_t, StoredBaseline> entries;
    std::size_t sweep_threshold = 256;
};

BaselineStore& baselines() {
    static BaselineStore store;
    return store;
}

// Drop entries whose View is gone. Runs when the store has doubled since the
// last sweep, so its size stays proportional to the live views it serves.
void sweep_expired(BaselineStore& store) {
    if (store.entries.size() < store.sweep_threshold)
        return;
    for (auto it = store.entries.begin(); it != store.entries.end();) {
        if (it->second.lifetime.expired())
            it = store.entries.erase(it);
        else
            ++it;
    }
    store.sweep_threshold = std::max<std::size_t>(256, store.entries.size() * 2);
}

// Copies the listed fields of a style struct from one instance to another.
template <typename Style, auto... Fields> void copy_fields(Style& to, const Style& from) {
    ((to.*Fields = from.*Fields), ...);
}

template <typename Style> struct SlotFields {
    std::string_view slot;
    void (*copy)(Style&, const Style&);
};

// The FlexStyle fields each setFlex key writes.
using F = FlexStyle;
constexpr SlotFields<F> kFlexSlots[] = {
    {"direction", copy_fields<F, &F::direction>},
    {"gap", copy_fields<F, &F::gap>},
    {"row_gap", copy_fields<F, &F::row_gap>},
    {"column_gap", copy_fields<F, &F::column_gap>},
    {"padding", copy_fields<F, &F::padding>},
    {"padding_top", copy_fields<F, &F::padding_top, &F::dim_padding_top>},
    {"padding_right", copy_fields<F, &F::padding_right, &F::dim_padding_right>},
    {"padding_bottom", copy_fields<F, &F::padding_bottom, &F::dim_padding_bottom>},
    {"padding_left", copy_fields<F, &F::padding_left, &F::dim_padding_left>},
    {"margin", copy_fields<F, &F::margin>},
    {"margin_top", copy_fields<F, &F::margin_top, &F::dim_margin_top>},
    {"margin_right", copy_fields<F, &F::margin_right, &F::dim_margin_right>},
    {"margin_bottom", copy_fields<F, &F::margin_bottom, &F::dim_margin_bottom>},
    {"margin_left", copy_fields<F, &F::margin_left, &F::dim_margin_left>},
    {"flex_grow", copy_fields<F, &F::flex_grow>},
    {"flex_shrink", copy_fields<F, &F::flex_shrink>},
    {"flex_basis", copy_fields<F, &F::flex_basis, &F::dim_flex_basis>},
    {"flex_wrap", copy_fields<F, &F::flex_wrap>},
    {"order", copy_fields<F, &F::order>},
    {"width", copy_fields<F, &F::preferred_width, &F::dim_width>},
    {"height", copy_fields<F, &F::preferred_height, &F::dim_height>},
    {"min_width", copy_fields<F, &F::min_width, &F::dim_min_width>},
    {"min_height", copy_fields<F, &F::min_height, &F::dim_min_height>},
    {"max_width", copy_fields<F, &F::max_width, &F::dim_max_width>},
    {"max_height", copy_fields<F, &F::max_height, &F::dim_max_height>},
    {"align_items", copy_fields<F, &F::align_items>},
    {"align_self", copy_fields<F, &F::align_self>},
    {"align_content", copy_fields<F, &F::align_content, &F::align_content_space>},
    {"justify_content", copy_fields<F, &F::justify_content>},
    {"aspect_ratio", copy_fields<F, &F::aspect_ratio>},
    {"box_sizing", copy_fields<F, &F::box_sizing>},
};

// The GridStyle fields each setGrid key writes. `grid_area` writes either
// the area name or all four placements.
using G = GridStyle;
constexpr SlotFields<G> kGridSlots[] = {
    {"template_columns", copy_fields<G, &G::template_columns>},
    {"template_rows", copy_fields<G, &G::template_rows>},
    {"template_areas", copy_fields<G, &G::template_areas>},
    {"auto_columns", copy_fields<G, &G::auto_columns>},
    {"auto_rows", copy_fields<G, &G::auto_rows>},
    {"auto_flow", copy_fields<G, &G::auto_flow>},
    {"gap", copy_fields<G, &G::column_gap, &G::row_gap>},
    {"column_gap", copy_fields<G, &G::column_gap>},
    {"row_gap", copy_fields<G, &G::row_gap>},
    {"column_start", copy_fields<G, &G::grid_column_start>},
    {"column_end", copy_fields<G, &G::grid_column_end>},
    {"row_start", copy_fields<G, &G::grid_row_start>},
    {"row_end", copy_fields<G, &G::grid_row_end>},
    {"grid_area", copy_fields<G, &G::grid_area_name, &G::grid_column_start, &G::grid_column_end,
                              &G::grid_row_start, &G::grid_row_end>},
};

// Copy the fields `slot` names from `from` into `to`. Returns false for a
// slot the table does not name.
template <typename Style, std::size_t N>
bool restore_slot(const SlotFields<Style> (&table)[N], std::string_view slot, Style& to,
                  const Style& from) {
    for (const auto& entry : table) {
        if (entry.slot != slot)
            continue;
        entry.copy(to, from);
        return true;
    }
    return false;
}

} // namespace

void capture_layout_baseline(const View& view) {
    std::lock_guard<std::mutex> lock(baseline_mutex());
    auto& store = baselines();
    // Every setFlex reaches here; only the first write per view copies.
    const auto id = view.import_binding_instance_id();
    if (store.entries.find(id) != store.entries.end())
        return;
    store.entries.emplace(id, StoredBaseline{LayoutBaseline{view.flex(), view.grid()},
                                             view.import_binding_lifetime_token()});
    sweep_expired(store);
}

std::optional<LayoutBaseline> find_layout_baseline(const View& view) {
    std::lock_guard<std::mutex> lock(baseline_mutex());
    const auto& entries = baselines().entries;
    const auto found = entries.find(view.import_binding_instance_id());
    if (found == entries.end())
        return std::nullopt;
    return found->second.baseline;
}

void forget_layout_baselines(const std::vector<std::uint64_t>& instance_ids) {
    std::lock_guard<std::mutex> lock(baseline_mutex());
    auto& entries = baselines().entries;
    if (entries.empty())
        return;
    for (const auto id : instance_ids)
        entries.erase(id);
}

std::size_t layout_baseline_count() {
    std::lock_guard<std::mutex> lock(baseline_mutex());
    return baselines().entries.size();
}

void BridgeRegistrars::register_style_reset_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // resetStyleProperty(id, slot) -> bool
    //
    // Restore one style slot to the value the widget had before the bridge
    // first wrote its layout. Slots are setFlex keys (`padding_top`,
    // `width`, `direction`, `box_sizing`, ...), setGrid keys prefixed with
    // `grid.` (`grid.template_columns`, `grid.grid_area`, ...), and
    // `animation`, which stops the CSS animations a setAnimation started.
    // Returns false for an unknown slot or widget, so a misspelt slot is
    // visible to the caller instead of silently doing nothing.
    register_bridge_function(
        api, "resetStyleProperty", [&self](choc::javascript::ArgumentList args) {
            const auto id = args.get<std::string>(0, "");
            const auto slot = args.get<std::string>(1, "");

            if (slot == "animation") {
                auto* v = id.empty() ? &self.root_ : self.widget(id);
                if (!v)
                    return choc::value::createBool(false);
                v->active_animations().clear();
                v->staged_animation() = View::StagedAnimation{};
                v->set_animation_play_state("running");
                return choc::value::createBool(true);
            }

            if (slot.rfind("grid.", 0) == 0) {
                auto* v = id.empty() ? &self.root_ : self.widget(id);
                if (!v)
                    return choc::value::createBool(false);
                GridStyle restored = v->grid();
                const auto baseline = find_layout_baseline(*v);
                const GridStyle& from = baseline ? baseline->grid : v->grid();
                if (!restore_slot(kGridSlots, std::string_view(slot).substr(5), restored, from))
                    return choc::value::createBool(false);
                v->grid() = std::move(restored);
                v->invalidate_layout();
                return choc::value::createBool(true);
            }

            // Flex slots resolve their target exactly as setFlex does, including
            // the retained-scroll split that routes padding to the inner box.
            View* v = id.empty() ? &self.root_ : self.style_target(id);
            if (!v)
                return choc::value::createBool(false);
            if (slot.rfind("padding", 0) == 0 && self.scroll_wrapper(id) != nullptr) {
                if (auto* content = self.widget(id))
                    v = content;
            }
            FlexStyle restored = v->flex();
            const auto baseline = find_layout_baseline(*v);
            const FlexStyle& from = baseline ? baseline->flex : v->flex();
            if (!restore_slot(kFlexSlots, slot, restored, from))
                return choc::value::createBool(false);
            v->flex() = restored;
            v->invalidate_layout();
            return choc::value::createBool(true);
        });
}

} // namespace pulp::view
