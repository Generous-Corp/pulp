#pragma once

/// @file native_surface_host.hpp
/// The `dev.pulp.native_surface/1` editor-host extension: what a host knows
/// about the window an editor draws into, for editors that bring their own
/// renderer and present into a native surface themselves.
///
/// An editor reaches it with `native_surface_host(context)`, or with
/// `EditorContext::query_extension(native_surface_extension_id)` cast to
/// `NativeSurfaceHost*`. A host that does not offer it returns null, and the
/// editor then relies only on what its own native view reports.
///
/// Every field reports what the host itself was told or decided, such as
/// the content scale a VST3 or CLAP host set or whether a host showed or
/// hid the editor's window. A field the host does not know reads as
/// unknown, so it is never confused with a value. An editor attached to a
/// Cocoa view can also read its own window, screen and occlusion state.
/// The host's answer adds what the native view cannot see, for example a
/// host-chosen scale on a platform where the window has none.
///
/// GPU device: an editor that uses this extension owns its GPU device (on
/// macOS a `MTLDevice` from the system, normally the one driving the window's
/// display) and presents into its own native layer. This is the one
/// exception to the rule that an editor acquires its GPU device from the
/// shared GPU runtime with a role tag. Such an editor never also uses the
/// shared runtime: one editor uses one device family, never both, because two
/// devices presenting into one window conflict on presentation and memory.
///
/// Threading:
/// - `NativeSurfaceHost` members and observer callbacks run on the host's
///   main thread, like the rest of the editor-host contract.
/// - An editor may render and present from a render thread it owns. That
///   thread may acquire and present its own drawables and read plain values
///   the main thread handed it. It must not call `EditorContext`,
///   `NativeSurfaceHost`, or `ParamPort::begin` / `set` / `end`, and it must not
///   touch the host's native views. `ParamPort::get` and `info` remain
///   callable from any thread.
/// - An editor stops its render thread before `Editor::detach()` or
///   `Editor::invalidate()` returns.

#include <pulp/format/editor_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::format {

/// The extension id. The layout of `NativeSurfaceHostInfo` and the members of
/// `NativeSurfaceHost` are fixed for this id; a change ships under a new id
/// (".../2"), which a host may offer beside this one.
inline constexpr std::string_view native_surface_extension_id = "dev.pulp.native_surface/1";

/// Whether the host shows the editor's window, as far as the host knows.
enum class NativeSurfaceVisibility : std::uint8_t {
    unknown = 0, ///< the host has said nothing; ask the native view
    visible = 1, ///< the host shows the window
    hidden = 2,  ///< the host hid the window without detaching the editor
};

/// One snapshot of what the host knows about the editor's window. Zero in a
/// numeric field means unknown.
struct NativeSurfaceHostInfo {
    /// Increases by one on every change, so a reader can tell two snapshots
    /// apart without comparing fields.
    std::uint64_t generation = 0;
    /// The display the window is on (a `CGDirectDisplayID` on macOS). 0 when
    /// the host does not know.
    std::uint32_t display_id = 0;
    /// The backing scale the host set for the editor (2.0 on a Retina display),
    /// or 0 when the host has set none.
    double backing_scale = 0.0;
    /// The display's nominal refresh rate in Hz, a pacing hint only. 0 when the
    /// host does not know.
    double refresh_hz = 0.0;
    NativeSurfaceVisibility visibility = NativeSurfaceVisibility::unknown;
};

/// Owns one `NativeSurfaceHost::observe()` registration. Destroying it, or
/// calling `reset()`, removes the observer. Move-only. Main thread.
class NativeSurfaceObservation {
  public:
    NativeSurfaceObservation() = default;
    /// Adopt a registration; `cancel` runs exactly once, on reset or
    /// destruction. Host implementations construct these.
    explicit NativeSurfaceObservation(std::function<void()> cancel) noexcept
        : cancel_(std::move(cancel)) {}
    NativeSurfaceObservation(NativeSurfaceObservation&& other) noexcept
        : cancel_(std::exchange(other.cancel_, {})) {}
    NativeSurfaceObservation& operator=(NativeSurfaceObservation&& other) noexcept {
        if (this != &other) {
            reset();
            cancel_ = std::exchange(other.cancel_, {});
        }
        return *this;
    }
    NativeSurfaceObservation(const NativeSurfaceObservation&) = delete;
    NativeSurfaceObservation& operator=(const NativeSurfaceObservation&) = delete;
    ~NativeSurfaceObservation() {
        reset();
    }

    /// Remove the observer now. Idempotent.
    void reset() noexcept {
        if (auto cancel = std::exchange(cancel_, {}))
            cancel();
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(cancel_);
    }

  private:
    std::function<void()> cancel_;
};

/// The interface behind `native_surface_extension_id`. Owned by the host and
/// valid while the `EditorContext` that returned it lives. Main thread.
class NativeSurfaceHost {
  public:
    /// What the host knows now.
    virtual NativeSurfaceHostInfo info() const = 0;

    /// Call `observer` on the main thread after each change, with the new
    /// snapshot. It is not called for the state at registration; read `info()`
    /// for that. An observer may reset its own or another observation from
    /// inside the call. The observation must not outlive the context.
    virtual NativeSurfaceObservation
    observe(std::function<void(const NativeSurfaceHostInfo&)> observer) = 0;

  protected:
    // An editor never owns or deletes the host's object.
    ~NativeSurfaceHost() = default;
};

/// The native-surface extension of `context`, or null when the host does not
/// offer it.
inline NativeSurfaceHost* native_surface_host(EditorContext& context) {
    return static_cast<NativeSurfaceHost*>(context.query_extension(native_surface_extension_id));
}

/// Returns `host` in the form `EditorContext::query_extension()` hands out.
/// A host must convert through this (or an equivalent cast to
/// `NativeSurfaceHost*` first), never return the address of a derived object
/// directly: an editor casts the pointer back to `NativeSurfaceHost*`.
inline void* as_extension(NativeSurfaceHost& host) {
    return static_cast<void*>(&host);
}

/// A host-side `NativeSurfaceHost` that records what an adapter reports and
/// tells observers. A host owns one per editor context and feeds it from its
/// own callbacks. Main thread.
class NativeSurfaceHostState final : public NativeSurfaceHost {
  public:
    NativeSurfaceHostState() = default;
    NativeSurfaceHostState(const NativeSurfaceHostState&) = delete;
    NativeSurfaceHostState& operator=(const NativeSurfaceHostState&) = delete;

    NativeSurfaceHostInfo info() const override {
        return info_;
    }

    NativeSurfaceObservation
    observe(std::function<void(const NativeSurfaceHostInfo&)> observer) override {
        if (!observer)
            return {};
        auto slot = std::make_shared<Slot>();
        slot->observer = std::move(observer);
        slots_.push_back(slot);
        std::weak_ptr<bool> alive = alive_;
        std::weak_ptr<Slot> weak_slot = slot;
        return NativeSurfaceObservation([this, alive, weak_slot] {
            const auto target = weak_slot.lock();
            if (!target)
                return;
            target->active = false;
            // An observation that outlives this object must not reach into it.
            if (alive.expired())
                return;
            slots_.erase(std::remove(slots_.begin(), slots_.end(), target), slots_.end());
        });
    }

    /// Each setter publishes a new generation only when the value changes.
    /// A non-finite or non-positive scale or rate reads as unknown (0).
    void set_backing_scale(double scale) {
        const double value = known_or_zero(scale);
        if (value == info_.backing_scale)
            return;
        info_.backing_scale = value;
        publish();
    }
    void set_display(std::uint32_t display_id, double refresh_hz) {
        const double rate = known_or_zero(refresh_hz);
        if (display_id == info_.display_id && rate == info_.refresh_hz)
            return;
        info_.display_id = display_id;
        info_.refresh_hz = rate;
        publish();
    }
    void set_visibility(NativeSurfaceVisibility visibility) {
        if (visibility == info_.visibility)
            return;
        info_.visibility = visibility;
        publish();
    }

    /// Number of live observations.
    std::size_t observer_count() const noexcept {
        return slots_.size();
    }

  private:
    struct Slot {
        std::function<void(const NativeSurfaceHostInfo&)> observer;
        bool active = true;
    };

    static double known_or_zero(double value) {
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }

    void publish() {
        ++info_.generation;
        // Observers may add or remove observations while being called, so walk
        // a copy and skip any slot cancelled since the walk began.
        const auto snapshot = slots_;
        const auto info = info_;
        for (const auto& slot : snapshot) {
            if (slot->active && slot->observer)
                slot->observer(info);
        }
    }

    NativeSurfaceHostInfo info_{};
    std::vector<std::shared_ptr<Slot>> slots_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace pulp::format
