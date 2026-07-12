#pragma once

/// @file host_frame_pump.hpp
/// The one frame-timing contract every host render loop must follow.
///
/// A host (native window, plugin-view host, foreign-host embed tick) drives the
/// view system from a vsync source — CVDisplayLink, CADisplayLink, an NSTimer,
/// or a test harness. Everything time-integrating downstream (FrameClock render
/// subscribers, CSS animation timelines, widget hover/thumb/scroll animations,
/// time-driven gesture recognizers, wake-from-idle activity probes) advances by
/// a `dt` the host hands it. Historically every Apple host handed them a
/// hardcoded `1.0f / 60.0f`, so on a 120 Hz ProMotion display animation time ran
/// at 2x wall clock, and under load (coalesced or dropped callbacks) it ran slow.
/// Motion duration became a function of the display and the machine.
///
/// `HostFramePump` turns the host's own monotonic presentation timestamp into a
/// measured dt; `begin_host_frame` / `advance_host_frame` then deliver that ONE
/// dt to every consumer, in the right order, identically on every platform.
///
/// Two deliberate design decisions:
///
///  1. **FrameClock is never clamped.** `FrameClock::time()` is the shader clock
///     (`iTime`-style uniforms read it) and the animation clock. A global clamp
///     silently desynchronises it from wall time — after a 500 ms hitch, a clamp
///     to 64 ms leaves the clock 436 ms behind, forever. So a *dropped or slow
///     frame while we were actively rendering* passes its raw measured dt
///     through: a 1 s fade still finishes ~1 s after it started even if a frame
///     hitched. Integrators catch up, which is what you want.
///
///  2. **Wake-from-idle is not a slow frame, and is the one place we do clamp.**
///     Pulp idles a static editor at 0 fps: the host stops pumping entirely.
///     When it resumes seconds (or minutes) later, the wall-clock gap is not
///     elapsed animation time — nothing was animating. Feeding it through would
///     teleport any animation that starts on wake straight to its end state on
///     its first frame. So a gap larger than `wake_threshold()` (default 250 ms
///     ≈ 15 dropped vsyncs at 60 Hz — far beyond any plausible frame hitch) is
///     reported as a *resume*: dt = `nominal_dt()` (one frame), and the pump
///     re-bases on the new timestamp. `suspend()` marks a resume explicitly when
///     the host knows it stopped (display link stopped, view detached).
///
///     The honest cost: a genuine >250 ms stall *while animating* is
///     indistinguishable from a wake at this seam and is treated as a resume, so
///     the clock falls behind by the stall. That is the conservative failure —
///     never teleport — and a stall that long has already destroyed the
///     animation visually.
///
///     The wake threshold alone is NOT enough, because a host can idle for far
///     less than it: the macOS display links keep firing while the tree is
///     static, and the host declines to dispatch those vsyncs to the UI thread
///     at all (`should_dispatch_host_frame`). The pump therefore stops measuring
///     while the tree is idle, and a 100 ms mouse-outside-the-window gap followed
///     by a hover animation would otherwise hand that animation a 100 ms first
///     frame — completing an 80 ms hover on frame one. So a host that SKIPS a
///     vsync must say so (`note_frame_skipped`), and the next measured frame is a
///     resume no matter how short the gap was. Idle time is never animation time,
///     at any scale.
///
/// Thread affinity: `begin_host_frame` / `advance_host_frame` are UI thread only
/// — they walk the view tree and fire callbacks without locking.
/// `should_dispatch_host_frame` / `HostFramePump::note_frame_skipped` are the one
/// exception: they are called from the vsync source's own thread (the
/// CVDisplayLink thread) and touch only an atomic flag.

#include <atomic>
#include <cstdint>

#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/frame_clock.hpp>

namespace pulp::view {

class View;

/// Converts a host's monotonic presentation timestamps into per-frame deltas.
/// Owned by the host, ticked on the UI thread. Prefer the vsync source's OWN
/// timestamp (CVTimeStamp::hostTime, CADisplayLink.targetTimestamp) over a fresh
/// wall-clock read: it is the time the frame will actually be presented, and it
/// stays correct even when the callback is delivered late.
class HostFramePump {
public:
    static constexpr float kDefaultNominalDt = 1.0f / 60.0f;
    static constexpr float kDefaultWakeThreshold = 0.25f;

    /// Measure the delta for the frame presented at `now_seconds` (monotonic,
    /// seconds). Returns, in order of precedence:
    ///   - `nominal_dt()` on the first frame and on any resume — a `suspend()`,
    ///     a pending `note_frame_skipped()`, or a gap over `wake_threshold()`
    ///     (see class docs),
    ///   - `0` for a non-advancing timestamp (duplicate / coalesced callback, or
    ///     a source that went backwards — never rewind the clock),
    ///   - otherwise the RAW measured delta, dropped frames included.
    float measure(double now_seconds);

    /// Tell the pump the host is about to stop pumping frames (idle at 0 fps,
    /// display link stopped, view detached). The next `measure()` is a resume
    /// regardless of the elapsed gap. Optional — the wake threshold catches an
    /// unannounced idle too — but explicit is exact.
    void suspend();

    /// Tell the pump the host had a vsync it deliberately did NOT turn into a
    /// frame (nothing dirty, nothing animating, no idle callback — see
    /// `should_dispatch_host_frame`). The pump did not measure that vsync, so the
    /// wall-clock gap it opens is idle time, not animation time: the next
    /// `measure()` is a resume regardless of how short the gap was.
    ///
    /// Callable from the vsync source's own thread (touches one atomic); cleared
    /// by the next `measure()` on the UI thread.
    void note_frame_skipped() { skipped_.store(true, std::memory_order_release); }

    /// Nominal frame interval used for the first frame and for resumes. Hosts
    /// that know their refresh period (CVDisplayLinkGetNominalOutputVideoRefreshPeriod,
    /// CADisplayLink.duration) should set it; the default assumes 60 Hz.
    void set_nominal_dt(float dt);
    float nominal_dt() const { return nominal_dt_; }

    /// Gap above which a frame is treated as a resume rather than a slow frame.
    void set_wake_threshold(float seconds);
    float wake_threshold() const { return wake_threshold_; }

    /// True while the next `measure()` would be a resume (before the first frame,
    /// after `suspend()`, or with a `note_frame_skipped()` still pending).
    bool suspended() const {
        return !have_last_ || skipped_.load(std::memory_order_acquire);
    }

    /// Number of resumes so far, first frame included. Observability + tests.
    std::uint64_t resumes() const { return resumes_; }

    /// Timestamp of the last measured frame (undefined while `suspended()`).
    double last_timestamp() const { return last_; }

    void reset();

private:
    double last_ = 0;
    bool have_last_ = false;
    float nominal_dt_ = kDefaultNominalDt;
    float wake_threshold_ = kDefaultWakeThreshold;
    std::uint64_t resumes_ = 0;
    // Set on the vsync thread by note_frame_skipped(), consumed on the UI thread
    // by measure(). The only cross-thread state in the pump.
    std::atomic<bool> skipped_{false};
};

/// Result of `begin_host_frame`.
struct HostFrameTick {
    /// The measured dt for this frame. EVERY time-integrating consumer this
    /// frame must be advanced with exactly this value.
    float dt = 0.0f;
    /// True if the host should composite this frame.
    bool should_render = false;
    /// True if the tree still wants continuous per-vsync frames (something is
    /// animating, or a render subscriber is live) — i.e. `should_render` would
    /// stay true next tick even without the host's own dirty flag. Hosts use
    /// this to keep their continuous-frame/dirty-tracker state armed. Computed
    /// once here so the tree walk happens exactly once per tick.
    bool continuous = false;
    /// True if this frame was a resume (first frame or wake-from-idle).
    bool resumed = false;
};

/// Step 0 of a host tick, on the VSYNC SOURCE'S OWN THREAD: does this vsync need
/// to reach the UI thread at all?
///
/// The macOS display links keep firing at the display's refresh rate even when
/// the tree is completely static. Rather than hop to the main thread 120x/sec to
/// walk a tree that cannot have changed, the gated hosts answer that question on
/// the link thread from three flags they maintain atomically: their own dirty
/// flag, whether the last dispatched frame reported `HostFrameTick::continuous`,
/// and whether a scripted idle callback is installed (a JS rAF / timer / async
/// queue must be pumped every vsync regardless of native dirtiness).
///
/// The frame-timing consequence, and the reason this lives here rather than being
/// open-coded per host: a skipped vsync is a vsync the pump never measured. If the
/// host just drops it, the pump's last timestamp goes stale and the NEXT real
/// frame measures the whole idle gap — so an animation starting on that frame is
/// advanced by the time the UI spent doing nothing (an 80 ms hover fade handed a
/// 100 ms first frame completes instantly). So the skip is recorded on the pump,
/// and the next measured frame is a resume. Idle time is never animation time.
///
/// Returns true if the host should dispatch a frame; false records the skip.
bool should_dispatch_host_frame(HostFramePump& pump, bool needs_repaint,
                                bool continuous, bool has_idle_callback);

/// Step 1 of a host tick: measure dt, pump the FrameClock's activity probes with
/// it, then decide whether this frame needs compositing.
///
/// Order matters: the activity probes run BEFORE the decision, so a probe that
/// flips a liveness flag this tick (a meter that just started moving calling
/// `set_continuous_repaint`) is reflected in this tick's answer instead of being
/// missed for a frame. `needs_repaint` is the host's own dirty flag (input,
/// resize, explicit invalidate). Null-safe on `root`.
HostFrameTick begin_host_frame(View* root, FrameClock& clock, HostFramePump& pump,
                               double now_seconds, bool needs_repaint);

/// Step 2 of a host tick, run only when `HostFrameTick::should_render`: advance
/// every dt-integrating consumer in the tree by the SAME dt — the render clock
/// (and its subscribers), widget animations, and CSS animation timelines — and
/// pump the gesture recognizers. Null-safe on `root`.
///
/// Gesture recognizers are deliberately NOT dt-driven: `advance_gesture_recognizers()`
/// takes an absolute monotonic timestamp (defaulted to its own `steady_clock` read)
/// and compares it against the timestamps stamped on the input events themselves,
/// so it already measures real time and must keep using ONE epoch with those
/// events. Feeding it the host's presentation clock would mix epochs.
void advance_host_frame(View* root, FrameClock& clock, float dt);

/// Advance the built-in widget animations and the CSS animation timeline for
/// `view` and every descendant. Shared by all hosts so the set of animated
/// widget types cannot drift per platform. Called by `advance_host_frame`;
/// public for hosts that drive widgets without a FrameClock.
void advance_widget_animations(View* view, float dt);

} // namespace pulp::view
