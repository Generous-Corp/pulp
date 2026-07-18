#include <pulp/view/host_param_surface.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/store.hpp>

namespace pulp::view {

void HostParamSurface::assert_call_context(const char* op) {
#ifndef NDEBUG
    // paint_all runs inside a ScopedNoAlloc; a host-surface call from paint
    // would re-enter arbitrary host formatter/getter code mid-render-traversal.
    // See the class-level threading contract.
    if (pulp::runtime::is_in_no_alloc_scope()) {
        std::fprintf(stderr,
                     "[pulp::view] HostParamSurface::%s called from paint()/no-alloc scope — "
                     "host-param access is legal only from tick/update. Snapshot values at "
                     "tick and paint from the snapshot.\n",
                     op);
        assert(false && "HostParamSurface call from paint()/no-alloc scope");
    }
#else
    (void)op;
#endif
}

void HostActionSurface::assert_call_context(const char* op) {
#ifndef NDEBUG
    if (pulp::runtime::is_in_no_alloc_scope()) {
        std::fprintf(stderr,
                     "[pulp::view] HostActionSurface::%s called from paint()/no-alloc scope — "
                     "host actions are legal only from tick/update.\n",
                     op);
        assert(false && "HostActionSurface call from paint()/no-alloc scope");
    }
#else
    (void)op;
#endif
}

// ── StateStoreHostParamSurface ─────────────────────────────────────────────

StateStoreHostParamSurface::StateStoreHostParamSurface(state::StateStore& store,
                                                       KeyResolver resolver)
    : StateStoreHostParamSurface(store, {}, std::move(resolver)) {}

StateStoreHostParamSurface::StateStoreHostParamSurface(
    state::StateStore& store, runtime::AliveToken::Handle owner_alive,
    KeyResolver resolver)
    : store_(store), owner_alive_(std::move(owner_alive)),
      resolver_(std::move(resolver)) {
    if (!resolver_) {
        // Default: match ParamInfo::name == key. Linear scan — a plugin's
        // parameter count is small, and this only runs on unresolved keys.
        resolver_ = [this](std::string_view key) -> std::optional<state::ParamID> {
            for (const auto& info : store_.all_params()) {
                if (info.name == key) return info.id;
            }
            return std::nullopt;
        };
    }
}

bool StateStoreHostParamSurface::owner_is_alive() const {
    return !owner_alive_ || runtime::AliveToken::is_alive(owner_alive_);
}

bool StateStoreHostParamSurface::do_has_param(std::string_view key) {
    if (!owner_is_alive()) return false;
    return resolver_(key).has_value();
}

double StateStoreHostParamSurface::do_get_param(std::string_view key) {
    if (!owner_is_alive()) return 0.0;
    if (auto id = resolver_(key)) return store_.get_normalized(*id);
    return 0.0;
}

void StateStoreHostParamSurface::do_set_param(std::string_view key, double normalized) {
    if (!owner_is_alive()) return;
    if (auto id = resolver_(key))
        store_.set_normalized(*id, static_cast<float>(normalized));
}

void StateStoreHostParamSurface::do_begin_gesture(std::string_view key) {
    if (!owner_is_alive()) return;
    if (auto id = resolver_(key)) store_.begin_gesture(*id);
}

void StateStoreHostParamSurface::do_end_gesture(std::string_view key) {
    if (!owner_is_alive()) return;
    if (auto id = resolver_(key)) store_.end_gesture(*id);
}

std::string StateStoreHostParamSurface::do_param_display_text(std::string_view key,
                                                              double normalized) {
    if (!owner_is_alive()) return {};
    auto id = resolver_(key);
    if (!id) return {};
    const state::ParamInfo* info = store_.info(*id);
    if (!info) return {};

    const float value = info->range.denormalize(static_cast<float>(normalized));
    if (info->to_string) return info->to_string(value);

    // Default numeric + unit fallback for params that declare no formatter.
    // %.3g keeps small integers clean ("440") while trimming float noise.
    char buf[64];
    if (!info->unit.empty())
        std::snprintf(buf, sizeof(buf), "%.3g %s", value, info->unit.c_str());
    else
        std::snprintf(buf, sizeof(buf), "%.3g", value);
    return std::string(buf);
}

} // namespace pulp::view
