// gpu_surface_observer.cpp — PluginViewHost's GPU-surface lifecycle
// notification (see plugin_view_host.hpp, "GPU-surface lifecycle").
//
// Why this exists at all: a host's GpuSurface is not available at a fixed point
// in the editor lifecycle. Apple and Linux build it in the host constructor, so
// a single read right after PluginViewHost::create() is correct there. Windows
// cannot — Dawn configures presentation for the HWND's native-window shape, and
// the editor HWND is a hidden WS_POPUP until attach_to_parent() reparents it
// into the DAW — so the same read returns null and never becomes non-null on
// its own. Every format adapter did that read, so on Windows the scripted UI
// kept a null surface (navigator.gpu fell back to mocks) and the CPU-fallback
// diagnostic fired on a host that was about to run on the GPU.
//
// Publishing transitions instead of exposing a pointer to poll also gives the
// detach edge somewhere to go: the host resets its surface on detach and on
// destruction, and consumers holding the raw pointer had no way to learn that.

#include <pulp/view/plugin_view_host.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace pulp::view {

namespace detail {

class GpuSurfaceObserverRegistry {
public:
    std::uint64_t add(PluginViewHost::GpuSurfaceObserver observer) {
        const std::uint64_t id = ++next_id_;
        entries_.push_back({id, std::move(observer)});
        return id;
    }

    void remove(std::uint64_t id) {
        // Erase during a dispatch is legal: notify() iterates a copy, and a
        // removed entry is filtered out of that copy by the id lookup below.
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [id](const Entry& e) { return e.id == id; }),
                       entries_.end());
    }

    void notify(const PluginViewHost::GpuSurfaceStatus& status) {
        // An observer may unsubscribe (or subscribe) from inside its own
        // callback — a scripted-UI teardown reacting to `unavailable` does
        // exactly that. Iterate a snapshot of the ids and re-look-up each one,
        // so a callback that mutates the list cannot invalidate this loop and
        // cannot invoke an observer that has already unsubscribed.
        std::vector<std::uint64_t> ids;
        ids.reserve(entries_.size());
        for (const Entry& e : entries_) ids.push_back(e.id);
        for (std::uint64_t id : ids) {
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [id](const Entry& e) { return e.id == id; });
            if (it == entries_.end()) continue;  // unsubscribed mid-dispatch
            auto observer = it->observer;        // copy: `it` may be invalidated
            if (observer) observer(status);
        }
    }

private:
    struct Entry {
        std::uint64_t id = 0;
        PluginViewHost::GpuSurfaceObserver observer;
    };
    std::vector<Entry> entries_;
    std::uint64_t next_id_ = 0;
};

}  // namespace detail

void PluginViewHost::GpuSurfaceSubscription::reset() noexcept {
    if (id_ == 0) return;
    if (auto registry = registry_.lock()) registry->remove(id_);
    registry_.reset();
    id_ = 0;
}

PluginViewHost::GpuSurfaceSubscription PluginViewHost::observe_gpu_surface(
    GpuSurfaceObserver observer) {
    if (!observer) return {};
    if (!gpu_observers_)
        gpu_observers_ = std::make_shared<detail::GpuSurfaceObserverRegistry>();

    // Copy the status BEFORE registering: the immediate delivery below is this
    // subscriber's view of "the state right now", and a callback that
    // subscribes another observer must not make the first one see a newer one.
    const GpuSurfaceStatus current = gpu_status_;
    const std::uint64_t id = gpu_observers_->add(std::move(observer));
    GpuSurfaceSubscription subscription(gpu_observers_, id);

    // Immediate delivery is what removes "did I subscribe too late?" from every
    // consumer. A subscriber attached after the surface already went ready gets
    // ready; one attached before gets pending and waits.
    gpu_observers_->notify(current);
    return subscription;
}

void PluginViewHost::publish_gpu_surface(render::GpuSurface* surface,
                                         GpuSurfaceState state) {
    // Normalize impossible combinations rather than propagating them. `ready`
    // is defined as "gpu_surface() is non-null"; a host that reports ready with
    // no surface would make every consumer's null check meaningless.
    if (state == GpuSurfaceState::ready && surface == nullptr)
        state = GpuSurfaceState::unavailable;
    if (state != GpuSurfaceState::ready) surface = nullptr;

    if (gpu_status_.surface == surface && gpu_status_.state == state) return;

    gpu_status_ = GpuSurfaceStatus{surface, state};
    if (gpu_observers_) gpu_observers_->notify(gpu_status_);
}

}  // namespace pulp::view
