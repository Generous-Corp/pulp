#pragma once

/// @file value_source_binding.hpp
/// View-side bindings for the paint-safe host→view channels in value_source.hpp.
///
/// A view that shows a live host value (a meter, a readout, a modulation ring)
/// needs the same three things every time: subscribe to the reachable FrameClock,
/// re-point that subscription when the clock changes, and tear it down without
/// dangling when the view or the clock goes away. These types are that machinery,
/// so a view binds a source instead of re-rolling the lifecycle.
///
/// Threading: the host publishes on the audio/host thread; everything here runs
/// on the UI thread. Each binding snapshots its source once per frame on the
/// clock callback and exposes the snapshot to `paint()` — the same
/// "snapshot at tick, paint from the snapshot" discipline `HostParamSurface`
/// enforces. A paint-time read is then a plain member read: no lock, no
/// allocation, no atomic.

#include <memory>

#include <pulp/view/value_source.hpp>

namespace pulp::view {

class View;
class FrameClock;

/// Subscription lifecycle for a view that pulls a value once per frame.
///
/// Owns one FrameClock render subscription on behalf of `owner`, keeps it
/// pointed at whatever clock the owner can currently reach, and drops it when
/// the owner detaches or the source goes away. A subscribed binding counts as a
/// render subscriber, so a bound-and-attached view keeps an editor's frames
/// alive (see FrameClock::has_active_subscribers).
///
/// The owner drives the binding from its own lifecycle: call `refresh()` when
/// the reachable clock may have changed, and `stop()` to unbind. `View` does
/// this for its built-in bindings; a view holding extra bindings of its own
/// (one per element, say) forwards `refresh()`/`stop()` the same way.
class FrameClockBinding {
public:
    /// Per-frame callback. `dt` is the clock delta in seconds. Return false to
    /// unsubscribe (the binding self-heals on the next frame either way).
    using Tick = bool (*)(void* context, float dt);

    FrameClockBinding(View& owner, Tick tick, void* context)
        : owner_(owner), tick_(tick), context_(context) {}
    ~FrameClockBinding() { stop(); }

    FrameClockBinding(const FrameClockBinding&) = delete;
    FrameClockBinding& operator=(const FrameClockBinding&) = delete;

    /// (Re)point the subscription at the owner's currently reachable clock.
    /// Idempotent. A no-op when no clock is reachable yet — a view built before
    /// hosting subscribes later, once a clock appears.
    void refresh(bool wanted);

    /// Unsubscribe from the cached clock, if subscribed. Safe to call
    /// repeatedly, and safe during destruction: it uses the cached clock
    /// pointer rather than walking the (possibly torn-down) parent chain.
    void stop();

    bool subscribed() const { return sub_id_ != -1; }
    FrameClock* subscribed_clock() const { return subscribed_clock_; }
    View& owner() const { return owner_; }

private:
    /// Clock callback: self-heal, then hand off to the owner's tick. Returning
    /// false auto-unsubscribes, which covers a detach that never fired
    /// `on_detached` on every descendant.
    bool on_frame(float dt);

    View& owner_;
    Tick tick_;
    void* context_;
    int sub_id_ = -1;                        // FrameClock subscription id, -1 = none
    FrameClock* subscribed_clock_ = nullptr; // cached for unsubscribe + detach detection
};

/// Binds a `MeterSource` channel and caches the latest frame for paint.
///
/// Lifetime: the bound FrameClock must outlive the binding, OR the host must
/// clear it (`root->set_frame_clock(nullptr)`) / detach the view before
/// destroying the clock — both drop the subscription first. Pulp's GPU hosts
/// clear the root clock during teardown, so this holds for them; a custom host
/// that owns a clock must observe the same order.
class MeterSourceBinding {
public:
    explicit MeterSourceBinding(View& owner)
        : binding_(owner, &MeterSourceBinding::tick, this) {}

    /// Bind a source (or nullptr to unbind, which drops the subscription so the
    /// view stops holding the editor's frames alive once nothing feeds it).
    void set_source(std::shared_ptr<MeterSource> source, int channel);

    /// Re-evaluate the subscription against the owner's reachable clock.
    void refresh() { binding_.refresh(static_cast<bool>(source_)); }
    void stop() { binding_.stop(); }

    /// The most recent frame snapshotted on the clock. Paint-safe: a plain
    /// member read, no lock and no allocation. Zeroed until the first frame.
    const MeterFrame& frame() const { return frame_; }

    bool has_source() const { return static_cast<bool>(source_); }
    int channel() const { return channel_; }
    bool subscribed() const { return binding_.subscribed(); }
    FrameClock* subscribed_clock() const { return binding_.subscribed_clock(); }

private:
    static bool tick(void* context, float dt);

    FrameClockBinding binding_;
    std::shared_ptr<MeterSource> source_;
    int channel_ = 0;
    MeterFrame frame_{};
};

/// Binds a `ScalarSource` and caches the latest value for paint. Same lifetime
/// contract as `MeterSourceBinding`.
class ScalarSourceBinding {
public:
    explicit ScalarSourceBinding(View& owner)
        : binding_(owner, &ScalarSourceBinding::tick, this) {}

    void set_source(std::shared_ptr<ScalarSource> source);

    void refresh() { binding_.refresh(static_cast<bool>(source_)); }
    void stop() { binding_.stop(); }

    /// The most recent value snapshotted on the clock. Paint-safe: a plain
    /// member read. 0 until the first frame.
    float value() const { return value_; }

    bool has_source() const { return static_cast<bool>(source_); }
    bool subscribed() const { return binding_.subscribed(); }
    FrameClock* subscribed_clock() const { return binding_.subscribed_clock(); }

private:
    static bool tick(void* context, float dt);

    FrameClockBinding binding_;
    std::shared_ptr<ScalarSource> source_;
    float value_ = 0.0f;
};

} // namespace pulp::view
