#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/quantize.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/clip.hpp>
#include <pulp/timeline/clip_launch.hpp>
#include <pulp/timeline/device_placement.hpp>
#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/note_modifier.hpp>
#include <pulp/timeline/recording.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

namespace detail {
class ProjectStateAccess;
class LauncherStore;
struct SequenceEditAccess;
} // namespace detail
/// Opaque node type retained by snapshot-owned Track::ClipView values.
struct ClipIndexNode;

/// Diagnostic counters for persistent track clip-index allocation.
struct TrackIndexStats {
    std::uint64_t live_nodes = 0;
    std::uint64_t nodes_created = 0;
};

/// Diagnostic counters for persistent launcher-index allocation.
struct LauncherIndexStats {
    std::uint64_t live_nodes = 0;
    std::uint64_t nodes_created = 0;
};

/// Complete input for constructing a Track and its owned collections.
struct TrackInput {
    ItemId id;
    std::string name;
    std::vector<Clip> clips;
    std::vector<DevicePlacement> device_chain;
    std::vector<AutomationLane> automation_lanes;
    std::vector<TakeLane> take_lanes;
    bool record_armed = false;
    // Zero selects the arrangement rather than a take playlist/comp lane.
    ItemId active_take_lane_id;
    std::optional<TrackFreeze> freeze;
    TrackMixer mixer;
};

/// Immutable identity-bearing arrangement track.
///
/// Owned collections use persistent storage. Successful edits return a new
/// Track and preserve this snapshot and all borrowed views into it.
class Track {
  public:
    /// Snapshot-owned random-access view of clips in timeline order.
    class ClipView {
      public:
        /// Forward iterator that keeps the underlying persistent index alive.
        class Iterator {
          public:
            using value_type = Clip;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;

            /// Borrows the current clip; retained index storage keeps it alive.
            const Clip& operator*() const noexcept;
            /// Returns the address of the current borrowed clip.
            const Clip* operator->() const noexcept;
            /// Advances in canonical timeline order.
            Iterator& operator++() noexcept;
            /// Compares iterator position and retained index identity.
            bool operator==(const Iterator&) const noexcept = default;

          private:
            friend class ClipView;
            Iterator(std::shared_ptr<const ClipIndexNode> root, std::size_t index) noexcept
                : root_(std::move(root)), index_(index) {}
            std::shared_ptr<const ClipIndexNode> root_;
            std::size_t index_ = 0;
        };

        /// Returns the number of clips in the view.
        std::size_t size() const noexcept {
            return size_;
        }
        /// Returns whether the view contains no clips.
        bool empty() const noexcept {
            return size_ == 0;
        }
        /// Returns the clip at `index`; precondition: `index < size()`.
        const Clip& operator[](std::size_t index) const noexcept;
        /// Returns an iterator to the first clip.
        Iterator begin() const noexcept {
            return Iterator(root_, 0);
        }
        /// Returns the sentinel iterator after the final clip.
        Iterator end() const noexcept {
            return Iterator(root_, size_);
        }

      private:
        friend class Track;
        ClipView(std::shared_ptr<const ClipIndexNode> root, std::size_t size) noexcept
            : root_(std::move(root)), size_(size) {}
        std::shared_ptr<const ClipIndexNode> root_;
        std::size_t size_ = 0;
    };

    /// Creates a track containing only its identity, name, and clips.
    static runtime::Result<Track, ModelError> create(ItemId id, std::string name,
                                                     std::vector<Clip> clips);
    /// Validates and creates a track from all authored collections.
    static runtime::Result<Track, ModelError> create(TrackInput input);
    /// Returns a snapshot with a validated, non-overlapping clip inserted.
    runtime::Result<Track, ModelError> insert_clip(Clip clip) const;
    /// Returns a snapshot without the identified clip, or MissingItem.
    runtime::Result<Track, ModelError> erase_clip(ItemId id) const;
    /// Replaces one clip by identity with O(log n) path-copy updates.
    ///
    /// The old Track remains valid; untouched index subtrees are shared.
    runtime::Result<Track, ModelError> replace_clip(Clip replacement) const;
    /// Returns a snapshot with a validated automation lane inserted.
    runtime::Result<Track, ModelError> insert_automation_lane(AutomationLane lane) const;
    /// Returns a snapshot without the identified automation lane.
    runtime::Result<Track, ModelError> erase_automation_lane(ItemId id) const;
    /// Returns a snapshot with a validated take lane inserted.
    runtime::Result<Track, ModelError> insert_take_lane(TakeLane lane) const;
    /// Removes a take lane unless it is currently selected.
    runtime::Result<Track, ModelError> erase_take_lane(ItemId id) const;
    /// Inserts a take into `lane_id` and returns the new track snapshot.
    runtime::Result<Track, ModelError> insert_take(ItemId lane_id, Take take) const;
    /// Removes a take unless the lane comp still references it.
    runtime::Result<Track, ModelError> erase_take(ItemId lane_id, ItemId take_id) const;
    /// Replaces one lane's comp with validated, canonical segments.
    runtime::Result<Track, ModelError> with_take_comp(ItemId lane_id,
                                                      std::vector<TakeCompSegment> comp) const;
    /// Returns a snapshot with record-arm intent set to `armed`.
    ///
    /// Owned collection storage is shared with the old Track.
    Track with_record_armed(bool armed) const;
    /// Selects arrangement when zero, or an existing take lane otherwise.
    runtime::Result<Track, ModelError> with_active_take_lane(ItemId lane_id) const;
    /// Publishes or clears a validated freeze artifact.
    runtime::Result<Track, ModelError> with_freeze(std::optional<TrackFreeze> freeze) const;
    /// Returns a snapshot with finite, in-range gain and pan.
    runtime::Result<Track, ModelError> with_mixer(TrackMixer mixer) const;

    /// Returns the stable track identity.
    ItemId id() const noexcept;
    /// Returns the authored track name.
    const std::string& name() const noexcept;
    /// Returns clips ordered by anchor, start, then identity.
    ClipView clips() const noexcept;
    /// Finds a clip by identity, or returns `nullptr`.
    ///
    /// Returned storage is snapshot-owned.
    const Clip* find_clip(ItemId id) const noexcept;
    /// Returns device placements in authored processing order.
    std::span<const DevicePlacement> device_chain() const noexcept;
    /// Finds a device placement by identity, or returns `nullptr`.
    const DevicePlacement* find_device_placement(ItemId id) const noexcept;
    /// Returns automation lanes in canonical identity order.
    std::span<const AutomationLane> automation_lanes() const noexcept;
    /// Finds an automation lane by identity, or returns `nullptr`.
    const AutomationLane* find_automation_lane(ItemId id) const noexcept;
    /// Returns take lanes in canonical identity order.
    std::span<const TakeLane> take_lanes() const noexcept;
    /// Finds a take lane by identity, or returns `nullptr`.
    const TakeLane* find_take_lane(ItemId id) const noexcept;
    /// Returns document intent for capture into a take lane.
    bool record_armed() const noexcept;
    /// Returns zero for arrangement playback or the selected take-lane identity.
    ItemId active_take_lane_id() const noexcept;
    /// Returns the published optional freeze artifact.
    const std::optional<TrackFreeze>& freeze() const noexcept;
    /// Returns the authored track mixer.
    const TrackMixer& mixer() const noexcept;
    /// Counts persistent clip-index nodes shared with `other`.
    std::size_t shared_index_nodes_with(const Track& other) const;
    /// Returns whether both snapshots contain the same clip identities.
    ///
    /// Clip content and timing may differ.
    bool shares_clip_membership_with(const Track& other) const noexcept;
    /// Returns whether both values reference the same complete track storage.
    bool shares_storage_with(const Track& other) const noexcept;
    /// Returns process-wide persistent clip-index diagnostic counters.
    static TrackIndexStats index_stats() noexcept;

  private:
    friend class Sequence;
    struct Data;
    bool shares_compile_structure_with(const Track& other) const noexcept;
    explicit Track(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

/// Harmonic quality of a chord statement.
///
/// Spelling and voicing are presentation and generation concerns.
enum class ChordQuality : std::uint8_t {
    Major,
    Minor,
    Diminished,
    Augmented,
    Dominant7,
    Major7,
    Minor7,
    HalfDiminished7,
    Suspended2,
    Suspended4,
};

/// Scale mode associated with a harmonic context event.
enum class ScaleMode : std::uint8_t {
    Major,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    Chromatic,
};

/// Harmonic statement in force from `position` until the next event.
///
/// Roots are pitch classes in [0, 11], where zero is C.
struct ChordScaleEvent {
    timebase::TickPosition position;
    ChordQuality chord_quality = ChordQuality::Major;
    std::uint8_t chord_root = 0;
    ScaleMode scale_mode = ScaleMode::Major;
    std::uint8_t scale_root = 0;

    constexpr auto operator<=>(const ChordScaleEvent&) const = default;
};

/// Immutable chord/scale context lane owned by a sequence.
///
/// Events are strictly ordered by position with no duplicates. The lane is
/// sequence-owned context read through the compile-context subscription
/// contract described in compile_context.hpp.
class ChordScaleLane {
  public:
    /// Validates, orders, and stores harmonic events.
    static runtime::Result<ChordScaleLane, ModelError> create(std::vector<ChordScaleEvent> events);

    /// Returns events in strictly increasing position order.
    std::span<const ChordScaleEvent> events() const noexcept {
        return *events_;
    }
    /// Returns whether no harmonic context is authored.
    bool empty() const noexcept {
        return events_->empty();
    }
    /// Returns the last event starting at or before `position`.
    ///
    /// Returns `nullptr` before the first event; no default harmony is invented.
    const ChordScaleEvent* at(timebase::TickPosition position) const noexcept;
    /// Returns whether both lanes share the same immutable event storage.
    bool shares_storage_with(const ChordScaleLane& other) const noexcept {
        return events_.get() == other.events_.get();
    }
    /// Compares authored event values.
    bool operator==(const ChordScaleLane& other) const noexcept;

  private:
    explicit ChordScaleLane(std::shared_ptr<const std::vector<ChordScaleEvent>> events) noexcept
        : events_(std::move(events)) {}

    std::shared_ptr<const std::vector<ChordScaleEvent>> events_;
};

/// Named point on a sequence's musical timeline.
///
/// Position is in canonical ticks. `color` is optional packed 0xRRGGBBAA;
/// absence delegates color choice to presentation.
struct SequenceMarker {
    ItemId id;
    std::string name;
    timebase::TickPosition position;
    std::optional<std::uint32_t> color;
};

/// Named positive-duration span on a sequence's musical timeline.
///
/// Regions may overlap or contain one another. Position and duration are in
/// canonical ticks; `color` has the same encoding as SequenceMarker::color.
struct SequenceRegion {
    ItemId id;
    std::string name;
    timebase::TickPosition position;
    timebase::TickDuration duration;
    std::optional<std::uint32_t> color;
};

/// Per-mille identity scale for deterministic groove strengths and velocity.
inline constexpr std::int32_t kGrooveUnitScale = 1000;
/// Largest admitted per-step groove velocity multiplier, in per-mille.
inline constexpr std::int32_t kMaxGrooveVelocityScale = 4000;
/// Largest admitted number of entries in a repeating groove table.
inline constexpr std::size_t kMaxGrooveSteps = 1024;

/// One timing offset and per-mille accent in a repeating groove table.
struct GrooveStep {
    timebase::TickDuration timing_offset{};
    std::int32_t velocity_scale = kGrooveUnitScale;

    constexpr auto operator<=>(const GrooveStep&) const = default;
};

/// Complete validated construction input for a GrooveTemplate.
struct GrooveTemplateInput {
    std::string name;
    // Zero means the groove states no swing. A swing grid is a subdivision:
    // the pair it warps spans twice this.
    timebase::TickDuration swing_grid{};
    timebase::SwingRatio swing = timebase::kStraightSwing;
    // The width of one entry in `steps`. Zero exactly when there is no table.
    timebase::TickDuration step{};
    std::vector<GrooveStep> steps;
    std::int32_t timing_strength = kGrooveUnitScale;
    std::int32_t velocity_strength = kGrooveUnitScale;
};

// A named feel a sequence plays with: a swing setting and a repeating table of
// per-step timing and accent, each scaled by its own strength.
//
// Groove carries velocity as well as timing deliberately. A feel is as much
// accent as it is placement — the same timing with flat velocities does not
// sound like the groove it was sampled from. Timing and accent share one
// canonical owned array. The two are separately
// attenuated so a project can borrow a groove's placement without its accents,
// which is the common editing request.
//
// Swing and the table are two independent displacements of the same authored
// position: the table is indexed by where a note was written, not by where
// swing moved it, so changing the swing setting never re-assigns notes to
// different steps.
//
// The swing half is order-preserving (see timebase::swing_position). The table
// half is not: adjacent steps may lean in opposite directions, and material at
// the end of one step can be pushed past material at the start of the next.
// That is what a groove template is, so offsets are bounded to less than a
// step rather than constrained into monotonicity.
//
// The template is sequence-owned context, not clip content: it is read by other
// items' compile hooks. Reading it across entities is what the compile-context
// subscription contract governs (see compile_context.hpp).
/// Immutable sequence-owned swing, timing-offset, and accent context.
class GrooveTemplate {
  public:
    /// Validates and stores one immutable groove template.
    static runtime::Result<GrooveTemplate, ModelError> create(GrooveTemplateInput input);

    /// Returns the authored display name.
    const std::string& name() const noexcept {
        return data_->name;
    }
    /// Returns the swung subdivision width in canonical ticks; zero disables swing.
    timebase::TickDuration swing_grid() const noexcept {
        return data_->swing_grid;
    }
    /// Returns the rational swing ratio.
    timebase::SwingRatio swing() const noexcept {
        return data_->swing;
    }
    /// Returns the repeating table-step width in canonical ticks; zero means no table.
    timebase::TickDuration step() const noexcept {
        return data_->step;
    }
    /// Returns authored groove steps in repeating order.
    std::span<const GrooveStep> steps() const noexcept {
        return data_->steps;
    }
    /// Returns timing-offset strength in per-mille.
    std::int32_t timing_strength() const noexcept {
        return data_->timing_strength;
    }
    /// Returns velocity-accent strength in per-mille.
    std::int32_t velocity_strength() const noexcept {
        return data_->velocity_strength;
    }
    /// Returns whether timing and velocity application are both identity operations.
    bool states_no_feel() const noexcept;
    /// Returns whether this is the exact default value carried by a new sequence.
    bool is_canonical_default() const noexcept;

    /// Returns the deterministic sounding tick for authored `position`.
    timebase::TickPosition apply_timing(timebase::TickPosition position) const noexcept;
    /// Returns the accent multiplier at `position`, in per-mille.
    std::int32_t velocity_scale_at(timebase::TickPosition position) const noexcept;

    /// Returns whether both templates share the same immutable backing data.
    bool shares_storage_with(const GrooveTemplate& other) const noexcept {
        return data_.get() == other.data_.get();
    }
    /// Compares complete authored groove values.
    bool operator==(const GrooveTemplate& other) const noexcept;

  private:
    // The table entry `position` falls in, or null when the groove has no table.
    const GrooveStep* step_at(timebase::TickPosition position) const noexcept;

    struct Data {
        std::string name;
        timebase::TickDuration swing_grid;
        timebase::SwingRatio swing;
        timebase::TickDuration step;
        std::vector<GrooveStep> steps;
        std::int32_t timing_strength;
        std::int32_t velocity_strength;

        bool operator==(const Data&) const = default;
    };
    explicit GrooveTemplate(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}

    std::shared_ptr<const Data> data_;
};

/// Complete construction input for a Sequence and its owned collections.
///
/// Absent context values select the canonical empty chord lane and no-feel groove.
struct SequenceInput {
    ItemId id;
    std::string name;
    std::optional<timebase::TickDuration> musical_duration;
    std::optional<AbsoluteTimelineDuration> absolute_duration;
    std::vector<Track> tracks;
    std::vector<SequenceMarker> markers;
    std::vector<SequenceRegion> regions;
    std::optional<ChordScaleLane> chord_scale_lane;
    std::optional<GrooveTemplate> groove;
    // Authored launch order. A scene owns its slots; every non-empty slot
    // references a clip in this sequence.
    std::vector<Scene> scenes;
};

/// Immutable identity-bearing arrangement with tracks, annotations, context, and launch scenes.
class Sequence {
  public:
    /// Snapshot-owned random-access view preserving authored scene order.
    class SceneView {
      public:
        /// Forward iterator that keeps the persistent launcher store alive.
        class Iterator {
          public:
            using value_type = Scene;
            using difference_type = std::ptrdiff_t;
            using pointer = const Scene*;
            using reference = const Scene&;
            using iterator_category = std::forward_iterator_tag;

            /// Borrows the current scene; retained launcher storage keeps it alive.
            const Scene& operator*() const noexcept;
            /// Returns the address of the current borrowed scene.
            const Scene* operator->() const noexcept;
            /// Advances in authored scene order.
            Iterator& operator++() noexcept;
            /// Advances in authored order and returns the prior iterator value.
            Iterator operator++(int) noexcept {
                auto copy = *this;
                ++*this;
                return copy;
            }
            /// Compares iterator position and retained launcher-store identity.
            bool operator==(const Iterator&) const noexcept = default;

          private:
            friend class SceneView;
            Iterator(std::shared_ptr<const detail::LauncherStore> store, ItemId current) noexcept
                : store_(std::move(store)), current_(current) {}
            std::shared_ptr<const detail::LauncherStore> store_;
            ItemId current_;
        };

        /// Returns the number of scenes.
        std::size_t size() const noexcept;
        /// Returns whether the view contains no scenes.
        bool empty() const noexcept {
            return size() == 0;
        }
        /// Returns the scene at `index`; precondition: `index < size()`.
        const Scene& operator[](std::size_t index) const noexcept;
        /// Returns an iterator to the first scene.
        Iterator begin() const noexcept;
        /// Returns the sentinel iterator after the final scene.
        Iterator end() const noexcept;

      private:
        friend class Sequence;
        explicit SceneView(std::shared_ptr<const detail::LauncherStore> store) noexcept
            : store_(std::move(store)) {}
        std::shared_ptr<const detail::LauncherStore> store_;
    };

    /// Creates a musical sequence with tracks and no annotations or launch scenes.
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
           std::vector<Track> tracks);
    /// Creates a mixed-domain sequence with tracks and explicit optional durations.
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks);
    /// Creates a sequence with annotations and canonical default context.
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
           std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions);
    /// Validates and creates a sequence from all authored collections.
    static runtime::Result<Sequence, ModelError> create(SequenceInput input);
    /// Creates a sequence with an explicit chord/scale context lane.
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
           std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions,
           ChordScaleLane chord_scale_lane);

    /// Returns the stable sequence identity.
    ItemId id() const noexcept;
    /// Returns the authored sequence name.
    const std::string& name() const noexcept;
    /// Returns the optional musical duration in canonical ticks.
    std::optional<timebase::TickDuration> duration() const noexcept;
    /// Returns the optional absolute duration and its rational rate.
    std::optional<AbsoluteTimelineDuration> absolute_duration() const noexcept;
    /// Returns tracks in canonical identity order.
    std::span<const Track> tracks() const noexcept;
    /// Returns sorted unique referenced sequence identities derived from clips.
    ///
    /// This index is not serialized.
    std::span<const ItemId> outgoing_sequence_refs() const noexcept;
    /// Finds a track by identity, or returns `nullptr`.
    const Track* find_track(ItemId id) const noexcept;
    /// Returns the always-present chord/scale context lane.
    const ChordScaleLane& chord_scale_lane() const noexcept;
    /// Returns the always-present groove context.
    const GrooveTemplate& groove() const noexcept;
    /// Returns markers ordered by position then identity.
    std::span<const SequenceMarker> markers() const noexcept;
    /// Returns regions ordered by position, duration, then identity.
    std::span<const SequenceRegion> regions() const noexcept;
    /// Returns authored scenes and slots in persistent snapshot-owned storage.
    SceneView scenes() const noexcept;
    /// Finds a marker by identity, or returns `nullptr`.
    const SequenceMarker* find_marker(ItemId id) const noexcept;
    /// Finds a region by identity, or returns `nullptr`.
    const SequenceRegion* find_region(ItemId id) const noexcept;
    /// Finds a scene by identity, or returns `nullptr`.
    const Scene* find_scene(ItemId id) const noexcept;
    /// Finds a slot across all scenes by identity, or returns `nullptr`.
    const Slot* find_slot(ItemId id) const noexcept;
    /// Returns a snapshot replacing an existing track by identity.
    runtime::Result<Sequence, ModelError> replace_track(Track track) const;
    /// Inserts a validated marker and returns a new snapshot.
    runtime::Result<Sequence, ModelError> insert_marker(SequenceMarker marker) const;
    /// Removes a marker by identity and returns a new snapshot.
    runtime::Result<Sequence, ModelError> erase_marker(ItemId id) const;
    /// Inserts a validated region and returns a new snapshot.
    runtime::Result<Sequence, ModelError> insert_region(SequenceRegion region) const;
    /// Removes a region by identity and returns a new snapshot.
    runtime::Result<Sequence, ModelError> erase_region(ItemId id) const;
    /// Inserts `scene` before an existing scene, or appends when no position is supplied.
    runtime::Result<Sequence, ModelError>
    insert_scene(Scene scene, std::optional<ItemId> before_scene_id = std::nullopt) const;
    /// Removes a scene and its owned slots by identity.
    runtime::Result<Sequence, ModelError> erase_scene(ItemId id) const;
    /// Inserts `slot` in `scene_id`, before an existing slot or at the end.
    runtime::Result<Sequence, ModelError>
    insert_slot(ItemId scene_id, Slot slot,
                std::optional<ItemId> before_slot_id = std::nullopt) const;
    /// Removes `slot_id` from `scene_id`.
    runtime::Result<Sequence, ModelError> erase_slot(ItemId scene_id, ItemId slot_id) const;
    /// Returns a snapshot with replacement harmonic context.
    Sequence with_chord_scale_lane(ChordScaleLane lane) const;
    /// Returns a snapshot with replacement groove context.
    Sequence with_groove(GrooveTemplate groove) const;
    /// Counts persistent launcher nodes shared with `other`.
    std::size_t shared_launcher_nodes_with(const Sequence& other) const;
    /// Returns whether both sequences share launcher collection storage.
    bool shares_launcher_storage_with(const Sequence& other) const noexcept;
    /// Returns whether both values share complete immutable sequence storage.
    bool shares_storage_with(const Sequence& other) const noexcept;
    /// Returns process-wide persistent launcher-index diagnostic counters.
    static LauncherIndexStats launcher_index_stats() noexcept;

  private:
    friend class Project;
    friend struct detail::SequenceEditAccess;
    struct Data;
    bool shares_compile_structure_with(const Sequence& other) const noexcept;
    // Annotation edits validate only the annotation lists and share the existing
    // track storage and identity index; they never re-walk the arrangement.
    runtime::Result<Sequence, ModelError>
    with_annotations(std::vector<SequenceMarker> markers,
                     std::vector<SequenceRegion> regions) const;
    explicit Sequence(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

/// Maximum admitted depth of an acyclic SequenceRef graph.
inline constexpr std::size_t kMaxSequenceNestingDepth = 8;

// Where this session's zero sits on the source/house clock — the document form
// of "this session starts at 01:00:00:00". Stored as an absolute sample offset
// paired with its own rational rate, never as a formatted timecode string:
// frame-rate formatting is a presentation concern and a string would make the
// same instant compare unequal across display rates.
/// Validates all SequenceRef targets, cycles, and maximum graph depth.
std::optional<ModelError>
validate_sequence_graph(std::span<const Sequence> sequences);
/// Validates one prospective parent-to-child edge against a sequence pool.
std::optional<ModelError>
validate_sequence_edge(std::span<const Sequence> sequences,
                       ItemId parent_id, ItemId child_id);

/// Absolute source/house-clock position corresponding to project time zero.
///
/// The value is stored in samples at an explicit rational rate; timecode
/// formatting remains a presentation concern.
struct SessionStart {
    timebase::SamplePosition start;
    timebase::RationalRate sample_rate;

    /// Compares the exact sample position and rational rate.
    constexpr bool operator==(const SessionStart& other) const noexcept {
        return start == other.start && sample_rate == other.sample_rate;
    }
};

/// Complete input for constructing a Project and its identity domain.
struct ProjectInput {
    ItemId id;
    std::string name;
    std::uint64_t next_item_id = 1;
    ItemId root_sequence_id;
    std::vector<MediaAsset> assets;
    std::vector<Sequence> sequences;
    timebase::TempoMap tempo_map{};
    timebase::MeterMap meter_map{};
    std::optional<SessionStart> session_start;
};

/// Kind of an identity-bearing item in a Project.
enum class ItemKind : std::uint8_t {
    Project,
    Asset,
    Sequence,
    Track,
    Clip,
    Note,
    DevicePlacement,
    AutomationLane,
    AutomationPoint,
    TakeLane,
    Take,
    Marker,
    Region,
    Scene,
    Slot,
};

/// Derives the canonical immediate owner from an item's kind and coordinates.
///
/// `lane_id` supplies the owning automation lane, take lane, or scene for
/// AutomationPoint, Take, and Slot respectively.
constexpr ItemId immediate_parent_id(ItemKind kind, ItemId project_id, ItemId sequence_id,
                                     ItemId track_id, ItemId clip_id,
                                     ItemId lane_id = {}) noexcept {
    switch (kind) {
    case ItemKind::Project:
        return {};
    case ItemKind::Asset:
    case ItemKind::Sequence:
        return project_id;
    case ItemKind::Track:
    case ItemKind::Marker:
    case ItemKind::Region:
    case ItemKind::Scene:
        return sequence_id;
    case ItemKind::Slot:
        return lane_id;
    case ItemKind::Clip:
    case ItemKind::DevicePlacement:
    case ItemKind::AutomationLane:
    case ItemKind::TakeLane:
        return track_id;
    case ItemKind::Note:
        return clip_id;
    case ItemKind::AutomationPoint:
    case ItemKind::Take:
        return lane_id;
    }
    return {};
}

/// Canonical ownership and ancestor coordinates for one project identity.
struct ItemLocation {
    ItemKind kind = ItemKind::Project;
    // Immediate ownership is canonical; the remaining IDs cache ancestor navigation.
    ItemId parent_id;
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    bool active = false;

    constexpr ItemLocation() noexcept = default;
    /// Creates one canonical location from its kind, immediate owner, ancestors, and state.
    constexpr ItemLocation(ItemKind item_kind, ItemId parent, ItemId sequence, ItemId track,
                           ItemId clip, bool is_active) noexcept
        : kind(item_kind), parent_id(parent), sequence_id(sequence), track_id(track), clip_id(clip),
          active(is_active) {}

    /// Returns whether two locations have the same kind and immediate owner.
    constexpr bool has_same_owner(const ItemLocation& other) const noexcept {
        return kind == other.kind && parent_id == other.parent_id;
    }
    constexpr auto operator<=>(const ItemLocation&) const = default;
};

/// Allowed transition applied to one identity-index entry.
enum class IdentityMutationKind : std::uint8_t { Insert, Deactivate, Reactivate };

/// Planned identity-index transition accompanying an immutable model edit.
struct IdentityMutation {
    IdentityMutationKind mutation = IdentityMutationKind::Insert;
    ItemId item;
    ItemLocation location;
};

/// Diagnostic counters for persistent project identity-index allocation.
struct ProjectIdentityStats {
    std::uint64_t live_nodes = 0;
    std::uint64_t nodes_created = 0;
};

/// Opaque, process-local identity for the part of a Project snapshot that
/// determines nested-sequence and registered-content compile subscribers.
class SequenceCompileStructureToken {
  public:
    SequenceCompileStructureToken() noexcept = default;

    bool valid() const noexcept {
        return value_ != 0;
    }

    constexpr bool operator==(const SequenceCompileStructureToken&) const noexcept = default;

  private:
    friend class Project;
    explicit SequenceCompileStructureToken(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_ = 0;
};

/// Immutable root of a Timeline document and its monotonic identity domain.
class Project {
  public:
    /// Validates referential integrity, ownership, durations, and identity monotonicity.
    static runtime::Result<Project, ModelError> create(ProjectInput input);

    /// Returns the stable project identity.
    ItemId id() const noexcept;
    /// Returns the authored project name.
    const std::string& name() const noexcept;
    /// Returns the next unallocated numeric ItemId.
    std::uint64_t next_item_id() const noexcept;
    /// Returns the identity of the arrangement's root sequence.
    ItemId root_sequence_id() const noexcept;
    /// Returns project-owned media assets in canonical identity order.
    std::span<const MediaAsset> assets() const noexcept;
    /// Returns project-owned sequences in canonical identity order.
    std::span<const Sequence> sequences() const noexcept;
    /// Returns the project's immutable tempo map.
    const timebase::TempoMap& tempo_map() const noexcept;
    /// Returns the project's immutable meter map.
    const timebase::MeterMap& meter_map() const noexcept;
    /// Returns the optional absolute source-clock origin.
    const std::optional<SessionStart>& session_start() const noexcept;
    /// Finds a media asset by identity, or returns `nullptr`.
    const MediaAsset* find_asset(ItemId id) const noexcept;
    /// Finds a sequence by identity, or returns `nullptr`.
    const Sequence* find_sequence(ItemId id) const noexcept;
    /// Returns canonical location data for active or inactive `id`, if known.
    std::optional<ItemLocation> locate(ItemId id) const noexcept;
    /// Counts persistent identity-index nodes shared with `other`.
    std::size_t shared_identity_nodes_with(const Project& other) const;
    /// Returns whether both values share complete immutable project storage.
    bool shares_storage_with(const Project& other) const noexcept;
    /// Process-local identity of the sequence-reference and registered-content
    /// placement shape used by incremental playback invalidation. The token is
    /// preserved across edits that cannot change dependency subscribers and is
    /// replaced before publishing a structurally incompatible snapshot.
    SequenceCompileStructureToken sequence_compile_structure_token() const noexcept;
    /// Returns process-wide persistent identity-index diagnostic counters.
    static ProjectIdentityStats identity_stats() noexcept;
    /// Returns an allocator initialized to this snapshot's identity frontier.
    ItemIdAllocator item_id_allocator() const noexcept {
        return ItemIdAllocator(next_item_id());
    }

  private:
    friend struct ProjectEditAccess;
    friend class detail::ProjectStateAccess;
    struct Data;
    runtime::Result<Project, ModelError>
    replace_sequence(Sequence sequence, std::span<const IdentityMutation> identities = {},
                     std::optional<std::uint64_t> next_item_id = std::nullopt) const;
    runtime::Result<Project, ModelError>
    append_sequence(Sequence sequence, std::span<const IdentityMutation> identities = {},
                    std::optional<std::uint64_t> next_item_id = std::nullopt) const;
    runtime::Result<Project, ModelError>
    remove_sequence(ItemId sequence_id, std::span<const IdentityMutation> identities = {}) const;
    // Appends a sealed media asset as a pinned project input. The asset carries
    // its own ContentHash identity; identity mutations register (or reactivate)
    // the ItemKind::Asset entry the same way clip inserts do.
    runtime::Result<Project, ModelError>
    append_asset(MediaAsset asset, std::span<const IdentityMutation> identities = {},
                 std::optional<std::uint64_t> next_item_id = std::nullopt) const;
    runtime::Result<Project, ModelError>
    remove_asset(ItemId asset_id, std::span<const IdentityMutation> identities = {}) const;
    Project replace_tempo_map(timebase::TempoMap tempo_map) const;
    Project replace_meter_map(timebase::MeterMap meter_map) const;
    explicit Project(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

/// Canonically ordered mapping from source identities to remapped identities.
class IdRemapTable {
  public:
    /// Returns mappings ordered by source identity.
    std::span<const std::pair<ItemId, ItemId>> entries() const noexcept {
        return entries_;
    }
    /// Returns the remapped identity for `old_id`, or `std::nullopt`.
    std::optional<ItemId> find(ItemId old_id) const noexcept;

  private:
    friend struct IdRemapBuilder;
    std::vector<std::pair<ItemId, ItemId>> entries_;
};

/// Optional non-throwing translation for an externally referenced identity.
///
/// A null callback preserves the identity. Callback failure makes the enclosing
/// remap fail without advancing its allocator.
struct ExternalIdFixup {
    void* context = nullptr;
    runtime::Result<ItemId, ModelError> (*map)(void*, ItemId) noexcept = nullptr;

    /// Applies the callback or returns `id` unchanged when none is installed.
    runtime::Result<ItemId, ModelError> apply(ItemId id) const noexcept;
};

/// Independent external-reference fixups for media assets and sequences.
struct RemapIdFixups {
    ExternalIdFixup asset;
    ExternalIdFixup sequence;
};

/// Remapped clip snapshot and its complete owned-identity table.
struct RemappedClip {
    Clip clip;
    IdRemapTable ids;
};
/// Remapped track snapshot and its complete owned-identity table.
struct RemappedTrack {
    Track track;
    IdRemapTable ids;
};
/// Remapped sequence snapshot and its complete owned-identity table.
struct RemappedSequence {
    Sequence sequence;
    IdRemapTable ids;
};

/// Remaps a clip's owned identities, optionally translating asset references.
runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    ExternalIdFixup external = {});
/// Remaps a clip with independent asset and sequence reference fixups.
runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    RemapIdFixups fixups);
/// Remaps a track's owned subtree, optionally translating asset references.
runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external = {});
/// Remaps a track with independent asset and sequence reference fixups.
runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     RemapIdFixups fixups);
/// Remaps a sequence's owned subtree, optionally translating asset references.
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external = {});
/// Remaps a sequence with independent asset and sequence reference fixups.
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, RemapIdFixups fixups);
/// Rebuilds a sequence from an explicit carried identity table and reference fixups.
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, std::span<const std::pair<ItemId, ItemId>> carried_ids,
          RemapIdFixups fixups = {});

/// Remapped project snapshot and complete source-to-destination identity table.
struct RemappedProject {
    Project project;
    IdRemapTable ids;
};

/// Remaps every project-owned identity starting at `first_id`.
///
/// The deterministic two-pass operation allocates all owned identities before
/// rebuilding references.
runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id);

/// @}

} // namespace pulp::timeline
