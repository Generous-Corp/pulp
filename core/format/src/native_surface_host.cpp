#include <pulp/format/native_surface_host.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::format {

struct NativeSurfaceHostState::Slot {
    std::function<void(const NativeSurfaceHostInfo&)> observer;
    bool active = true;
};

namespace {

double known_or_zero(double value) {
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

} // namespace

NativeSurfaceHostState::~NativeSurfaceHostState() {
    // An observation that outlives this object must not reach back into it.
    *alive_ = false;
}

NativeSurfaceObservation
NativeSurfaceHostState::observe(std::function<void(const NativeSurfaceHostInfo&)> observer) {
    if (!observer)
        return {};
    auto slot = std::make_shared<Slot>();
    slot->observer = std::move(observer);
    slots_.push_back(slot);
    std::weak_ptr<bool> alive = alive_;
    std::weak_ptr<Slot> weak_slot = slot;
    return NativeSurfaceObservation([this, alive, weak_slot] {
        auto still_alive = alive.lock();
        auto target = weak_slot.lock();
        if (!target)
            return;
        target->active = false;
        if (!still_alive || !*still_alive)
            return;
        slots_.erase(std::remove(slots_.begin(), slots_.end(), target), slots_.end());
    });
}

std::size_t NativeSurfaceHostState::observer_count() const noexcept {
    return slots_.size();
}

void NativeSurfaceHostState::set_backing_scale(double scale) {
    const double value = known_or_zero(scale);
    if (value == info_.backing_scale)
        return;
    info_.backing_scale = value;
    publish();
}

void NativeSurfaceHostState::set_display(std::uint32_t display_id, double refresh_hz) {
    const double rate = known_or_zero(refresh_hz);
    if (display_id == info_.display_id && rate == info_.refresh_hz)
        return;
    info_.display_id = display_id;
    info_.refresh_hz = rate;
    publish();
}

void NativeSurfaceHostState::set_visibility(NativeSurfaceVisibility visibility) {
    if (visibility == info_.visibility)
        return;
    info_.visibility = visibility;
    publish();
}

void NativeSurfaceHostState::publish() {
    ++info_.generation;
    // Observers may add or remove observations while being called, so walk a
    // copy and skip any slot cancelled since the walk began.
    const auto snapshot = slots_;
    const auto info = info_;
    for (const auto& slot : snapshot) {
        if (slot->active && slot->observer)
            slot->observer(info);
    }
}

} // namespace pulp::format
