#pragma once

#include <pulp/playback/clip_launch.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline/command.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples::timeline_session {

/// One resolved launch, recorded as the session performs. `start` and `end` are
/// the monotonic beats the launch and its stop actually resolved to, so a
/// captured performance inherits the launch machinery's accuracy rather than a
/// position recovered from a sample offset.
struct LaunchRecord {
    timeline::ItemId slot_id;
    timeline::ItemId track_id;
    timeline::ItemId clip_id;
    timebase::TickPosition start{0};
    timebase::TickDuration duration{0};
};

/// Capturing a performance is a flatten expressed in the model: every recorded
/// launch becomes an ordinary `InsertClip` against the track's arrangement lane.
/// No subsystem hop, no launcher-specific command — the arrangement that results
/// is indistinguishable from one authored by hand.
///
/// `next_item_id` is the project's id cursor; each emitted clip mints one id
/// from it, and the cursor advances so the caller can write it back.
std::vector<timeline::Command> flatten_launch_history(const timeline::Sequence& sequence,
                                                      std::span<const LaunchRecord> history,
                                                      std::uint64_t& next_item_id);

/// Worked example (d): a clip-launching session. Three tracks populate slot
/// lists, two scenes group them into rows, and per-track arbitration selects
/// launcher-versus-arrangement providers. The arrangement lanes hold the clips a
/// slot references but are not what plays while a track's provider is Launcher.
class TimelineLaunchSession {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    TimelineLaunchSession();
    ~TimelineLaunchSession();
    TimelineLaunchSession(const TimelineLaunchSession&) = delete;
    TimelineLaunchSession& operator=(const TimelineLaunchSession&) = delete;

    bool prepare(double sample_rate, std::uint32_t maximum_block_size);

    /// Arm every slot in `scene_id`, quantized by each slot's authored grid.
    /// Control-thread call; the launches resolve on subsequent process blocks.
    bool launch_scene(timeline::ItemId scene_id) noexcept;
    /// Arm a stop for every sounding slot, quantized by `quantize`.
    void stop_all(timeline::LaunchQuantize quantize) noexcept;

    /// Advance one block. Allocation-free after prepare.
    void process(std::uint32_t frames) noexcept;

    /// Point a track at the launcher or back at its arrangement lane. Recompiles
    /// through the compiler's provider-arbitration primitive rather than any
    /// launcher-specific path.
    bool set_track_provider(timeline::ItemId track_id, playback::ProviderKind provider);
    playback::ProviderKind track_provider(timeline::ItemId track_id) const noexcept;

    const std::vector<LaunchRecord>& history() const noexcept;
    const timeline::Project& project() const noexcept;
    const timebase::CompiledTempoMap& tempo_map() const noexcept;
    /// Apply `commands` through the ordinary transaction path and adopt the
    /// result. Used to commit a captured performance into the arrangement.
    bool apply(std::span<const timeline::Command> commands);
    std::size_t sounding_slot_count() const noexcept;
    /// Launches that resolved but could not be recorded because the history was
    /// at its reserved capacity. Non-zero means the capture is incomplete — the
    /// example reports it rather than letting a truncation pass as a full take.
    std::size_t dropped_capture_count() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::examples::timeline_session
