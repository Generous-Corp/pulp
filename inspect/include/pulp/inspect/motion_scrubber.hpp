// motion_scrubber.hpp — Timeline scrubber that replays a .motion.jsonl
// fixture frame-by-frame against an inspector client.
//
// The scrubber is passive: it does not run animations live and does not
// advance a clock. It loads a recorded fixture into memory, exposes a
// playhead (frame index), and re-emits the prefix of events with
// `frame <= playhead` to a caller-supplied Sink each time the playhead
// moves. This is sufficient for design-review timeline scrubbing and CI
// artifact triage; live overlay drawing belongs to a separate layer.
//
// Inspector dispatch:
//   Motion.loadFixture { path }    → load events, reset playhead to 0
//   Motion.scrubTo     { frame }   → set playhead, re-emit prefix
//   Motion.play        {}          → mark playing, jump to max frame
//   Motion.pause       {}          → clear playing flag
//   Motion.snapshot    {}          → playhead + loaded state
//
// The scrubber is off by default — `loaded()` is false until
// `load_fixture()` succeeds. Wiring is analogous to MotionInspector:
// DomainHandler owns the instance and routes Motion.* requests to it
// when the method is a scrubber method.

#pragma once

#include <pulp/inspect/protocol.hpp>
#include <pulp/view/motion.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::inspect {

class InspectorServer;

class MotionScrubber {
public:
    /// Construct with an optional InspectorServer. With a server,
    /// re-emitted events are broadcast as Motion.start / .sample / .end
    /// messages on the wire (the same shape MotionInspector uses).
    /// Without a server, events still flow to any sinks installed via
    /// `add_sink`.
    explicit MotionScrubber(InspectorServer* server = nullptr);
    ~MotionScrubber();

    MotionScrubber(const MotionScrubber&) = delete;
    MotionScrubber& operator=(const MotionScrubber&) = delete;

    /// Handle a Motion.* scrubber request. Returns response message.
    /// Caller (DomainHandler) decides routing — this method assumes the
    /// request is one of the scrubber methods.
    InspectorMessage handle(const InspectorMessage& req);

    /// True when at least one of the scrubber methods is the natural
    /// target for `req.method`. DomainHandler uses this to decide
    /// whether to route to MotionScrubber vs MotionInspector.
    static bool owns_method(const std::string& method);

    // ── Direct API (used by tests + advanced clients) ───────────────

    /// Load a fixture file. Returns true on success (any event count
    /// is acceptable; an empty fixture is loaded as zero events). Resets
    /// the playhead to 0 and clears the previously-loaded events.
    bool load_fixture(const std::string& path);

    /// Move the playhead to the given frame and re-emit the prefix of
    /// events with `frame <= playhead`. Returns the number of events
    /// emitted (zero for an unloaded scrubber). Backwards scrubs are
    /// supported — each call re-emits from the start.
    std::size_t scrub_to(std::uint64_t frame);

    /// Set playing flag, advance to the last loaded frame, emit
    /// everything. Passive: no real-time pacing.
    std::size_t play();

    /// Clear the playing flag. No event emission. Playhead unchanged.
    void pause();

    /// Install a sink that receives re-emitted events. Returns a
    /// non-zero id usable with `remove_sink`.
    int add_sink(view::motion::Sink sink);
    void remove_sink(int id);

    // ── Read-only accessors ─────────────────────────────────────────

    bool loaded() const;
    bool playing() const;
    std::uint64_t playhead_frame() const;
    std::size_t event_count() const;
    std::uint64_t max_frame() const;
    view::motion::FixtureHeader header() const;

private:
    InspectorServer* server_ = nullptr;

    mutable std::mutex mtx_;
    std::vector<view::motion::SampleEvent> events_;
    view::motion::FixtureHeader header_{};
    std::uint64_t playhead_frame_ = 0;
    std::uint64_t max_frame_ = 0;
    bool loaded_ = false;
    bool playing_ = false;

    struct SinkSlot {
        int id = 0;
        view::motion::Sink sink;
    };
    std::vector<SinkSlot> sinks_;
    int next_sink_id_ = 1;

    // Protocol handlers.
    InspectorMessage handle_load_fixture(const InspectorMessage& req);
    InspectorMessage handle_scrub_to(const InspectorMessage& req);
    InspectorMessage handle_play(const InspectorMessage& req);
    InspectorMessage handle_pause(const InspectorMessage& req);

    // Dispatch a snapshot of events to the given sinks + server WITHOUT
    // holding mtx_. Callers must collect the snapshot under the lock and
    // then call this. Sinks are free to re-enter MotionScrubber methods.
    static void dispatch_snapshot(
        const std::vector<view::motion::SampleEvent>& events,
        const std::vector<SinkSlot>& sinks,
        InspectorServer* server);
};

} // namespace pulp::inspect
