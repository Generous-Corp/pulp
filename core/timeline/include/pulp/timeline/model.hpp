#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/quantize.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/clip_launch.hpp>
#include <pulp/timeline/device_placement.hpp>
#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/note_modifier.hpp>

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

namespace detail {
class ProjectStateAccess;
class LauncherStore;
struct SequenceEditAccess;
} // namespace detail
class SchemaRegistry;

enum class ModelErrorCode : std::uint8_t {
    InvalidItemId,
    DuplicateItemId,
    ItemIdExhausted,
    InvalidDuration,
    InvalidMediaRange,
    InvalidSampleRate,
    InvalidNote,
    OverlappingClips,
    MissingAsset,
    MissingRootSequence,
    NextItemIdNotMonotonic,
    MixedTimeAnchors,
    IncompatibleSampleRate,
    MissingItem,
    IdentityConflict,
    InvalidIdentityTransition,
    InvalidSchemaIdentity,
    InvalidContentHash,
    InvalidAssetLocator,
    DuplicateAssetRepresentation,
    InvalidOpaqueContent,
    OpaqueContentLimitExceeded,
    OpaqueContentCannotRemap,
    InvalidClipPlaybackProperties,
    MissingAutomationTarget,
    DuplicateAutomationTarget,
    InvalidTake,
    DuplicateTake,
    ActiveTakeLaneRemoval,
    InvalidTakeComp,
    OverlappingTakeComp,
    ActiveCompTakeRemoval,
    InvalidAudioLoopInfo,
    InvalidAssetStoragePolicy,
    InvalidMarker,
    InvalidRegion,
    InvalidSessionStart,
    InvalidChordScaleEvent,
    UnorderedChordScaleLane,
    InvalidGrooveTemplate,
    InvalidNoteModifier,
    MissingSequenceReference,
    SequenceReferenceCycle,
    SequenceNestingTooDeep,
};

struct ModelError {
    ModelErrorCode code = ModelErrorCode::InvalidItemId;
    ItemId item;
    ItemId related_item;
};

struct SchemaIdentity {
    std::string type_name;
    std::uint32_t version = 0;

    bool valid() const noexcept;
    auto operator<=>(const SchemaIdentity&) const = default;
};

class ItemIdAllocator {
  public:
    explicit constexpr ItemIdAllocator(std::uint64_t next = 1) noexcept : next_(next) {}

    runtime::Result<ItemId, ModelError> allocate() noexcept;
    constexpr std::uint64_t next_value() const noexcept {
        return next_;
    }

  private:
    std::uint64_t next_ = 1;
};

struct EmptyContent {};

struct MediaAsset {
    ItemId id;
    std::string name;
    std::uint64_t frame_count = 0;
    timebase::RationalRate sample_rate;
    ContentHash content_hash;
    AssetStoragePolicy storage_policy = AssetStoragePolicy::External;
    std::vector<AssetLocator> locators;
    std::vector<AssetRepresentation> representations;
    std::optional<AudioLoopInfo> loop_info;
};

struct MediaRef {
    ItemId asset_id;
    timebase::SamplePosition source_start;
    std::uint64_t frame_count = 0;
};

// A reference to another sequence in the same Project pool. The referenced
// sequence remains a project-owned sibling: this clip owns neither the
// sequence nor any of its identities.
struct SequenceRef {
    ItemId sequence_id;
    timebase::TickPosition source_start{0};

    constexpr auto operator<=>(const SequenceRef&) const = default;
};

enum class ClipTimeAnchor : std::uint8_t { Musical, Absolute };

struct MusicalTimeRange {
    timebase::TickPosition start;
    timebase::TickDuration duration;
};

struct AbsoluteTimeRange {
    timebase::SamplePosition start;
    std::uint64_t sample_count = 0;
    timebase::RationalRate sample_rate;
};

struct AbsoluteTimelineDuration {
    std::uint64_t sample_count = 0;
    timebase::RationalRate sample_rate;
};

/// Clip-level audio controls. Fade lengths use the clip anchor's native
/// unit: canonical ticks for musical clips and timeline samples for absolute
/// clips. The playback compiler resolves both to sample-exact frame counts.
struct ClipPlaybackProperties {
    float gain_linear = 1.0f;
    std::uint64_t fade_in_duration = 0;
    std::uint64_t fade_out_duration = 0;
    constexpr auto operator<=>(const ClipPlaybackProperties&) const = default;
};

using ClipTimeRange = std::variant<MusicalTimeRange, AbsoluteTimeRange>;

struct NoteEvent {
    ItemId id;
    timebase::TickPosition start;
    timebase::TickDuration duration;
    std::uint16_t velocity = 0xffff;
    std::uint8_t pitch = 60;
    std::uint8_t channel = 0;
};

class NoteContent {
  public:
    /// Notes alone: no modifiers, and a zero seed. Every note plays once,
    /// unconditionally.
    static runtime::Result<NoteContent, ModelError> create(std::vector<NoteEvent> notes);
    /// Notes plus their sparse modifier companion array. Each modifier must
    /// name a note in `notes`, must be well-formed, and must not be neutral —
    /// a neutral entry is a second encoding of a note that already plays that
    /// way. `modifier_seed` is the document's authored seed for probability
    /// draws; the same seed always reproduces the same decisions.
    static runtime::Result<NoteContent, ModelError> create(std::vector<NoteEvent> notes,
                                                           std::vector<NoteModifier> modifiers,
                                                           std::uint64_t modifier_seed);
    runtime::Result<NoteContent, ModelError> replace_note(NoteEvent note) const;

    std::span<const NoteEvent> notes() const noexcept {
        return data_->notes;
    }

    /// Sorted by note id and containing only notes whose playback differs from
    /// the default, so a document that authors no modifiers carries none.
    std::span<const NoteModifier> modifiers() const noexcept {
        return data_->modifiers;
    }

    std::uint64_t modifier_seed() const noexcept {
        return data_->modifier_seed;
    }

    /// The modifier for `note_id`, or nullptr when the note plays by default.
    const NoteModifier* modifier_for(ItemId note_id) const noexcept;

  private:
    struct Data {
        std::vector<NoteEvent> notes;
        std::vector<NoteModifier> modifiers;
        std::uint64_t modifier_seed = 0;
    };

    explicit NoteContent(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

    std::shared_ptr<const Data> data_;
};

// Registered content is an extension-defined typed C++ value. The schema
// registry owns serialization; the model deliberately stores no generic
// string-keyed property bag. Registered payloads must own no ItemIds, so
// subtree remapping can preserve them without hidden reference corruption.
class RegisteredContent {
  public:
    const SchemaIdentity& schema() const noexcept {
        return schema_;
    }
    const std::shared_ptr<const void>& value() const noexcept {
        return value_;
    }
    const std::string& canonical_payload_json() const noexcept {
        return canonical_payload_json_;
    }
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }

    template <typename T> const T* value_as() const noexcept {
        return static_cast<const T*>(value_.get());
    }

  private:
    friend class SchemaRegistry;
    RegisteredContent(SchemaIdentity schema, std::shared_ptr<const void> value,
                      std::string canonical_payload_json, std::size_t retained_bytes)
        : schema_(std::move(schema)), value_(std::move(value)),
          canonical_payload_json_(std::move(canonical_payload_json)),
          retained_bytes_(retained_bytes) {}
    SchemaIdentity schema_;
    std::shared_ptr<const void> value_;
    std::string canonical_payload_json_;
    std::size_t retained_bytes_ = 0;
};

// Opaque extension envelopes retain the exact parser bounds under which they
// were admitted. This keeps a caller-selected trust boundary stable when the
// immutable value is later serialized again.
struct OpaqueContentLimits {
    std::size_t max_input_bytes = 1024ull * 1024ull * 1024ull;
    std::size_t max_depth = 64;
    std::size_t max_total_values = 30'000'000;
    std::size_t max_array_elements = 10'000'000;
    std::size_t max_object_members = 4'096;
    std::size_t max_string_bytes = 16ull * 1024ull * 1024ull;
    std::size_t max_opaque_bytes = 64ull * 1024ull * 1024ull;

    constexpr auto operator<=>(const OpaqueContentLimits&) const = default;
};

// Unknown extension content remains an exact validated JSON envelope. It is
// safe to retain and re-save but cannot be copied/imported because its internal
// identity/reference shape is unavailable.
class OpaqueContent {
  public:
    static runtime::Result<OpaqueContent, ModelError>
    create(SchemaIdentity schema, std::string raw_json, OpaqueContentLimits limits = {});

    const SchemaIdentity& schema() const noexcept {
        return schema_;
    }
    const std::string& raw_json() const noexcept {
        return raw_json_;
    }
    const OpaqueContentLimits& validation_limits() const noexcept {
        return limits_;
    }

  private:
    OpaqueContent(SchemaIdentity schema, std::string raw_json, OpaqueContentLimits limits)
        : schema_(std::move(schema)), raw_json_(std::move(raw_json)), limits_(limits) {}
    SchemaIdentity schema_;
    std::string raw_json_;
    OpaqueContentLimits limits_;
};

using ClipContent = std::variant<EmptyContent, MediaRef, NoteContent, RegisteredContent,
                                 OpaqueContent, SequenceRef>;

/// Overload set for visiting a ClipContent with **no generic fallback**.
///
/// What a clip *is* decides whether it renders audio, contributes notes, owns
/// ItemIds that must be remapped, or survives a save. Every one of those
/// decisions is a per-alternative dispatch, and none of them has a defensible
/// default: a content kind nobody wrote a branch for is not "nothing", it is a
/// clip whose data was dropped. Consumers that dispatch through a generic
/// lambda (`[](const auto&)`, or an `if`/`if constexpr` chain that falls
/// through) keep compiling when the variant grows and then quietly treat the
/// new alternative as absent — a clip that renders silence, an export manifest
/// that reports no loss while losing data, a remap that leaves stale ItemIds
/// behind. Visiting through this type makes a new alternative a compile error
/// at every call site until someone decides what it means.
///
///     std::visit(ClipContentCases{
///                    [&](const EmptyContent&) { ... },
///                    [&](const MediaRef& media) { ... },
///                    [&](const NoteContent& notes) { ... },
///                    [&](const RegisteredContent& registered) { ... },
///                    [&](const OpaqueContent& opaque) { ... },
///                },
///                clip.content());
template <class... Fs> struct ClipContentCases : Fs... {
    using Fs::operator()...;
};
template <class... Fs> ClipContentCases(Fs...) -> ClipContentCases<Fs...>;

/// Guard for code that can only be correct while ClipContent holds exactly the
/// alternatives it does today — a decoder keyed on envelope type names, a
/// referential-integrity scan that assumes MediaRef is the only alternative
/// naming an asset, an audio path that assumes MediaRef is the only alternative
/// carrying samples. Those sites cannot be expressed as a visit, so they assert
/// on the alternative count instead: widening the variant stops the build with a
/// message naming the decision that site owes, rather than shipping a document
/// that silently loses the new content on load, save, or render.
inline constexpr std::size_t kClipContentAlternativeCount = std::variant_size_v<ClipContent>;

class Clip {
  public:
    static runtime::Result<Clip, ModelError> create(ItemId id, timebase::TickPosition start,
                                                    timebase::TickDuration duration,
                                                    ClipContent content,
                                                    ClipPlaybackProperties playback = {});
    static runtime::Result<Clip, ModelError>
    create_absolute(ItemId id, timebase::SamplePosition start, std::uint64_t sample_count,
                    timebase::RationalRate sample_rate, ClipContent content,
                    ClipPlaybackProperties playback = {});

    ItemId id() const noexcept;
    ClipTimeAnchor time_anchor() const noexcept;
    const ClipTimeRange& time_range() const noexcept;
    timebase::TickPosition start() const noexcept;
    timebase::TickDuration duration() const noexcept;
    timebase::TickPosition end() const noexcept;
    timebase::SamplePosition absolute_start() const noexcept;
    std::uint64_t absolute_duration_samples() const noexcept;
    timebase::RationalRate absolute_sample_rate() const noexcept;
    timebase::SamplePosition absolute_end() const noexcept;
    const ClipContent& content() const noexcept;
    runtime::Result<Clip, ModelError> with_time_range(ClipTimeRange range) const;
    runtime::Result<Clip, ModelError> with_content(ClipContent content) const;
    runtime::Result<Clip, ModelError>
    with_playback_properties(ClipPlaybackProperties playback) const;
    ClipPlaybackProperties playback_properties() const noexcept;

  private:
    struct Data;
    explicit Clip(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

struct ClipIndexNode;

struct TrackIndexStats {
    std::uint64_t live_nodes = 0;
    std::uint64_t nodes_created = 0;
};

struct LauncherIndexStats {
    std::uint64_t live_nodes = 0;
    std::uint64_t nodes_created = 0;
};

// A Take is one recorded region that references a sealed media asset. It lives
// in a TakeLane on a Track and is anchored to absolute (sample) time: raw
// captures are sample-accurate against the transport, not musical. The take's
// timeline length is its media frame_count. A Take owns a stable ItemId because
// comps (a follow-up) select take segments by identity. The asset reference is
// external (Project::create validates it exists); the take never owns the asset.
class Take {
  public:
    static runtime::Result<Take, ModelError> create(ItemId id, MediaRef media,
                                                    timebase::SamplePosition placement_start,
                                                    timebase::RationalRate sample_rate);

    ItemId id() const noexcept {
        return id_;
    }
    const MediaRef& media() const noexcept {
        return media_;
    }
    timebase::SamplePosition placement_start() const noexcept {
        return placement_start_;
    }
    timebase::RationalRate sample_rate() const noexcept {
        return sample_rate_;
    }

  private:
    Take(ItemId id, MediaRef media, timebase::SamplePosition placement_start,
         timebase::RationalRate sample_rate) noexcept
        : id_(id), media_(media), placement_start_(placement_start), sample_rate_(sample_rate) {}

    ItemId id_;
    MediaRef media_;
    timebase::SamplePosition placement_start_;
    timebase::RationalRate sample_rate_;
};

static_assert(std::is_nothrow_copy_constructible_v<Take>);
static_assert(std::is_nothrow_move_constructible_v<Take>);

// One exact absolute-time selection in a take comp. The selected timeline range
// must lie inside the referenced take and use its normalized sample rate.
struct TakeCompSegment {
    ItemId take_id;
    AbsoluteTimeRange range;
    constexpr bool operator==(const TakeCompSegment& other) const noexcept {
        return take_id == other.take_id && range.start == other.range.start &&
               range.sample_count == other.range.sample_count &&
               range.sample_rate == other.range.sample_rate;
    }
};

// A track freeze selects a sealed offline-rendered asset without discarding the
// authored track. The render-plan hash identifies the exact immutable inputs
// used to derive the artifact, so callers can detect and replace stale caches.
struct TrackFreeze {
    MediaRef media;
    timebase::SamplePosition placement_start;
    timebase::RationalRate sample_rate;
    ContentHash render_plan_hash;
    constexpr bool operator==(const TrackFreeze& other) const noexcept {
        return media.asset_id == other.media.asset_id &&
               media.source_start == other.media.source_start &&
               media.frame_count == other.media.frame_count &&
               placement_start == other.placement_start && sample_rate == other.sample_rate &&
               render_plan_hash == other.render_plan_hash;
    }
};

// Immutable ownership of one alternate recording lane on a Track: an ordered
// set of takes keyed by identity and an optional sample-exact comp assembled
// from non-overlapping take segments. The comp is document intent; playback can
// derive and cache a flattened artifact without changing this source data.
class TakeLane {
  public:
    static runtime::Result<TakeLane, ModelError> create(ItemId id, std::string name,
                                                        std::vector<Take> takes,
                                                        std::vector<TakeCompSegment> comp = {});
    runtime::Result<TakeLane, ModelError> insert_take(Take take) const;
    runtime::Result<TakeLane, ModelError> erase_take(ItemId id) const;
    runtime::Result<TakeLane, ModelError>
    with_comp_segments(std::vector<TakeCompSegment> comp) const;

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    // Canonical order by take identity; carries no playback semantics.
    std::span<const Take> takes() const noexcept;
    // Canonical order by timeline start, then take identity.
    std::span<const TakeCompSegment> comp_segments() const noexcept;
    const Take* find_take(ItemId id) const noexcept;

  private:
    struct Data;
    explicit TakeLane(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

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
};

class Track {
  public:
    class ClipView {
      public:
        class Iterator {
          public:
            using value_type = Clip;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;

            const Clip& operator*() const noexcept;
            const Clip* operator->() const noexcept;
            Iterator& operator++() noexcept;
            bool operator==(const Iterator&) const noexcept = default;

          private:
            friend class ClipView;
            Iterator(std::shared_ptr<const ClipIndexNode> root, std::size_t index) noexcept
                : root_(std::move(root)), index_(index) {}
            std::shared_ptr<const ClipIndexNode> root_;
            std::size_t index_ = 0;
        };

        std::size_t size() const noexcept {
            return size_;
        }
        bool empty() const noexcept {
            return size_ == 0;
        }
        const Clip& operator[](std::size_t index) const noexcept;
        Iterator begin() const noexcept {
            return Iterator(root_, 0);
        }
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

    static runtime::Result<Track, ModelError> create(ItemId id, std::string name,
                                                     std::vector<Clip> clips);
    static runtime::Result<Track, ModelError> create(TrackInput input);
    runtime::Result<Track, ModelError> insert_clip(Clip clip) const;
    runtime::Result<Track, ModelError> erase_clip(ItemId id) const;
    // Replaces one clip by identity with O(log n) path-copy updates. The old
    // Track remains valid and unchanged; untouched index subtrees are shared.
    runtime::Result<Track, ModelError> replace_clip(Clip replacement) const;
    runtime::Result<Track, ModelError> insert_automation_lane(AutomationLane lane) const;
    runtime::Result<Track, ModelError> erase_automation_lane(ItemId id) const;
    runtime::Result<Track, ModelError> insert_take_lane(TakeLane lane) const;
    runtime::Result<Track, ModelError> erase_take_lane(ItemId id) const;
    runtime::Result<Track, ModelError> insert_take(ItemId lane_id, Take take) const;
    runtime::Result<Track, ModelError> erase_take(ItemId lane_id, ItemId take_id) const;
    runtime::Result<Track, ModelError> with_take_comp(ItemId lane_id,
                                                      std::vector<TakeCompSegment> comp) const;
    // Sets the record-arm intent flag. Path-copies only the Track's own Data;
    // clip, automation, and take index storage is shared with the old Track.
    Track with_record_armed(bool armed) const;
    runtime::Result<Track, ModelError> with_active_take_lane(ItemId lane_id) const;
    runtime::Result<Track, ModelError> with_freeze(std::optional<TrackFreeze> freeze) const;

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    // Ordered by (anchor, start, id) over a persistent AVL timeline index.
    ClipView clips() const noexcept;
    // Binary-searches the persistent ID index. Returned storage is snapshot-owned.
    const Clip* find_clip(ItemId id) const noexcept;
    // Preserves authored processing order. Returned storage is snapshot-owned.
    std::span<const DevicePlacement> device_chain() const noexcept;
    const DevicePlacement* find_device_placement(ItemId id) const noexcept;
    // Automation lane order is canonical by identity and carries no processing semantics.
    std::span<const AutomationLane> automation_lanes() const noexcept;
    const AutomationLane* find_automation_lane(ItemId id) const noexcept;
    // Take-lane order is canonical by identity and carries no processing semantics.
    std::span<const TakeLane> take_lanes() const noexcept;
    const TakeLane* find_take_lane(ItemId id) const noexcept;
    // Whether this track is armed to capture into a take lane. Pure document
    // intent; the capture engine reads it but never mutates it here.
    bool record_armed() const noexcept;
    // Zero means the arrangement is active. A non-zero value always names one
    // of this track's take lanes; full segment-comp data remains a later layer.
    ItemId active_take_lane_id() const noexcept;
    const std::optional<TrackFreeze>& freeze() const noexcept;
    std::size_t shared_index_nodes_with(const Track& other) const;
    // True when both snapshots are proven to contain exactly the same clip
    // identities. Clip content/timing may differ.
    bool shares_clip_membership_with(const Track& other) const noexcept;
    bool shares_storage_with(const Track& other) const noexcept;
    static TrackIndexStats index_stats() noexcept;

  private:
    friend class Sequence;
    struct Data;
    bool shares_compile_structure_with(const Track& other) const noexcept;
    explicit Track(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

// The harmonic quality of a chord symbol on the context lane. Spelling is
// deliberately absent: the lane states harmony, not notation, so a renderer
// derives voicings from (root, quality) rather than from an authored note set.
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

// The scale a passage is drawn from. A chord names the vertical, a scale names
// the horizontal; a generator following the lane usually needs both, and one
// does not determine the other (the same chord sits in several scales).
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

// One harmonic statement, in force from `position` until the next event. Roots
// are pitch classes (0 = C .. 11 = B), not MIDI notes: the lane says what the
// harmony is, never which octave to play it in.
struct ChordScaleEvent {
    timebase::TickPosition position;
    ChordQuality chord_quality = ChordQuality::Major;
    std::uint8_t chord_root = 0;
    ScaleMode scale_mode = ScaleMode::Major;
    std::uint8_t scale_root = 0;

    constexpr auto operator<=>(const ChordScaleEvent&) const = default;
};

// The chord/scale context lane a sequence carries. Events are strictly ordered
// by position with no duplicates, so "the harmony at tick t" is a single
// well-defined answer rather than a resolution policy.
//
// The lane is sequence-owned context, not clip content: it is read by other
// items' compile hooks. Reading it across entities is what the compile-context
// subscription contract governs (see compile_context.hpp) — a renderer declares
// that it reads this lane, and the compiler dirties exactly those declared
// readers when the lane is edited.
class ChordScaleLane {
  public:
    static runtime::Result<ChordScaleLane, ModelError> create(std::vector<ChordScaleEvent> events);

    std::span<const ChordScaleEvent> events() const noexcept {
        return *events_;
    }
    bool empty() const noexcept {
        return events_->empty();
    }
    // The event in force at `position`: the last one starting at or before it.
    // Null before the first event — the lane states harmony where it was
    // authored and never invents a default key.
    const ChordScaleEvent* at(timebase::TickPosition position) const noexcept;
    bool shares_storage_with(const ChordScaleLane& other) const noexcept {
        return events_.get() == other.events_.get();
    }
    bool operator==(const ChordScaleLane& other) const noexcept;

  private:
    explicit ChordScaleLane(std::shared_ptr<const std::vector<ChordScaleEvent>> events) noexcept
        : events_(std::move(events)) {}

    std::shared_ptr<const std::vector<ChordScaleEvent>> events_;
};

// A marker is a named point on the sequence timeline; a region is a named span.
// Both are anchored in canonical ticks — the same musical domain a musical clip
// uses — and both carry a stable ItemId so a command can address one by identity.
// A region is not a clip: it holds no content and never renders. When the
// sequence declares a musical duration, a marker's position and a region's whole
// span must lie inside it.
//
// Both are owned by the Sequence, not the Project. A Project holds many
// sequences, so a project-level annotation list could not say which timeline it
// annotates; sequence ownership makes the annotated timeline structural rather
// than conventional.
//
// `color` is a packed 0xRRGGBBAA value, not a float colour: the document model
// stores exact bytes that compare and re-serialize identically, and the float
// colour types live in render layers outside this module's dependency floor.
// It is optional — absent means the presentation layer chooses.
struct SequenceMarker {
    ItemId id;
    std::string name;
    timebase::TickPosition position;
    std::optional<std::uint32_t> color;
};

// Regions MAY overlap, including full containment. Named sections nest by
// nature ("chorus" inside "part B"), so disjointness is deliberately not an
// invariant: Sequence::create accepts any set of in-bounds, positive-length,
// uniquely identified regions. The only ordering guarantee is the canonical
// sort below.
struct SequenceRegion {
    ItemId id;
    std::string name;
    timebase::TickPosition position;
    timebase::TickDuration duration;
    std::optional<std::uint32_t> color;
};

// Scales are per-mille integers rather than floats because a groove is
// document data whose effect is a tick and a velocity: two machines must agree
// on the result exactly, and a float would make that agreement depend on
// rounding.
inline constexpr std::int32_t kGrooveUnitScale = 1000;
inline constexpr std::int32_t kMaxGrooveVelocityScale = 4000;
inline constexpr std::size_t kMaxGrooveSteps = 1024;

// One step of a groove's table: how far material inside that step moves, and
// how hard it is played relative to what was authored.
struct GrooveStep {
    timebase::TickDuration timing_offset{};
    std::int32_t velocity_scale = kGrooveUnitScale;

    constexpr auto operator<=>(const GrooveStep&) const = default;
};

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
// sound like the groove it was sampled from — and modelling accent now costs
// one array rather than a second schema version later. The two are separately
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
class GrooveTemplate {
  public:
    static runtime::Result<GrooveTemplate, ModelError> create(GrooveTemplateInput input);

    const std::string& name() const noexcept {
        return data_->name;
    }
    timebase::TickDuration swing_grid() const noexcept {
        return data_->swing_grid;
    }
    timebase::SwingRatio swing() const noexcept {
        return data_->swing;
    }
    timebase::TickDuration step() const noexcept {
        return data_->step;
    }
    std::span<const GrooveStep> steps() const noexcept {
        return data_->steps;
    }
    std::int32_t timing_strength() const noexcept {
        return data_->timing_strength;
    }
    std::int32_t velocity_strength() const noexcept {
        return data_->velocity_strength;
    }
    // True when the groove displaces nothing: no swing and no table. This is
    // what a sequence carries when it states no feel, and applying it is the
    // identity on every position and every velocity.
    bool states_no_feel() const noexcept;
    // True only for the canonical default serialized by a newly created
    // sequence. Authored metadata and controls count even when they currently
    // produce no audible displacement.
    bool is_canonical_default() const noexcept;

    // Where material authored at `position` sounds.
    timebase::TickPosition apply_timing(timebase::TickPosition position) const noexcept;
    // The accent multiplier for material authored at `position`, in per-mille
    // of the authored velocity.
    std::int32_t velocity_scale_at(timebase::TickPosition position) const noexcept;

    bool shares_storage_with(const GrooveTemplate& other) const noexcept {
        return data_.get() == other.data_.get();
    }
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

// The full construction surface for a Sequence, and the one to extend from here
// on. The positional create() overloads below predate it and remain as thin
// forwarders so no existing call site had to change; a new owned collection
// belongs in this struct rather than in a ninth argument.
//
// The two context members are optional only so the struct stays
// default-constructible for designated initializers — neither ChordScaleLane
// nor GrooveTemplate has a default constructor, because both validate on
// creation. Absent means the default the sequence would otherwise carry: a lane
// stating no harmony, and a groove stating no feel.
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

class Sequence {
  public:
    class SceneView {
      public:
        class Iterator {
          public:
            using value_type = Scene;
            using difference_type = std::ptrdiff_t;
            using pointer = const Scene*;
            using reference = const Scene&;
            using iterator_category = std::forward_iterator_tag;

            const Scene& operator*() const noexcept;
            const Scene* operator->() const noexcept;
            Iterator& operator++() noexcept;
            Iterator operator++(int) noexcept {
                auto copy = *this;
                ++*this;
                return copy;
            }
            bool operator==(const Iterator&) const noexcept = default;

          private:
            friend class SceneView;
            Iterator(std::shared_ptr<const detail::LauncherStore> store, ItemId current) noexcept
                : store_(std::move(store)), current_(current) {}
            std::shared_ptr<const detail::LauncherStore> store_;
            ItemId current_;
        };

        std::size_t size() const noexcept;
        bool empty() const noexcept {
            return size() == 0;
        }
        const Scene& operator[](std::size_t index) const noexcept;
        Iterator begin() const noexcept;
        Iterator end() const noexcept;

      private:
        friend class Sequence;
        explicit SceneView(std::shared_ptr<const detail::LauncherStore> store) noexcept
            : store_(std::move(store)) {}
        std::shared_ptr<const detail::LauncherStore> store_;
    };

    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
           std::vector<Track> tracks);
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks);
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
           std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions);
    // Sequence is pimpl'd behind a shared Data, so a new owned collection never
    // forces a layout change on a call site. The positional overloads above are
    // the historical arities and forward into this one; groove was the next
    // owned collection, and the arity note they carried said to stop adding
    // arguments at that point, so groove and scenes live in SequenceInput.
    static runtime::Result<Sequence, ModelError> create(SequenceInput input);
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks,
           std::vector<SequenceMarker> markers, std::vector<SequenceRegion> regions,
           ChordScaleLane chord_scale_lane);

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    std::optional<timebase::TickDuration> duration() const noexcept;
    std::optional<AbsoluteTimelineDuration> absolute_duration() const noexcept;
    std::span<const Track> tracks() const noexcept;
    // Sorted, unique, derived from SequenceRef clips, and never serialized.
    std::span<const ItemId> outgoing_sequence_refs() const noexcept;
    const Track* find_track(ItemId id) const noexcept;
    // Always present; empty when the sequence states no harmony.
    const ChordScaleLane& chord_scale_lane() const noexcept;
    // Always present; states no feel when the sequence plays straight.
    const GrooveTemplate& groove() const noexcept;
    // Ordered by (position, id). Markers and regions share one identity space:
    // no marker may reuse a region's ItemId within the same sequence.
    std::span<const SequenceMarker> markers() const noexcept;
    // Ordered by (position, duration, id). Overlap is permitted.
    std::span<const SequenceRegion> regions() const noexcept;
    SceneView scenes() const noexcept;
    const SequenceMarker* find_marker(ItemId id) const noexcept;
    const SequenceRegion* find_region(ItemId id) const noexcept;
    const Scene* find_scene(ItemId id) const noexcept;
    const Slot* find_slot(ItemId id) const noexcept;
    runtime::Result<Sequence, ModelError> replace_track(Track track) const;
    runtime::Result<Sequence, ModelError> insert_marker(SequenceMarker marker) const;
    runtime::Result<Sequence, ModelError> erase_marker(ItemId id) const;
    runtime::Result<Sequence, ModelError> insert_region(SequenceRegion region) const;
    runtime::Result<Sequence, ModelError> erase_region(ItemId id) const;
    runtime::Result<Sequence, ModelError>
    insert_scene(Scene scene, std::optional<ItemId> before_scene_id = std::nullopt) const;
    runtime::Result<Sequence, ModelError> erase_scene(ItemId id) const;
    runtime::Result<Sequence, ModelError>
    insert_slot(ItemId scene_id, Slot slot,
                std::optional<ItemId> before_slot_id = std::nullopt) const;
    runtime::Result<Sequence, ModelError> erase_slot(ItemId scene_id, ItemId slot_id) const;
    Sequence with_chord_scale_lane(ChordScaleLane lane) const;
    Sequence with_groove(GrooveTemplate groove) const;
    std::size_t shared_launcher_nodes_with(const Sequence& other) const;
    bool shares_launcher_storage_with(const Sequence& other) const noexcept;
    bool shares_storage_with(const Sequence& other) const noexcept;
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

inline constexpr std::size_t kMaxSequenceNestingDepth = 8;

// Where this session's zero sits on the source/house clock — the document form
// of "this session starts at 01:00:00:00". Stored as an absolute sample offset
// paired with its own rational rate, never as a formatted timecode string:
// frame-rate formatting is a presentation concern and a string would make the
// same instant compare unequal across display rates.
struct SessionStart {
    timebase::SamplePosition start;
    timebase::RationalRate sample_rate;

    constexpr bool operator==(const SessionStart& other) const noexcept {
        return start == other.start && sample_rate == other.sample_rate;
    }
};

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

// Canonical immediate parent for a kind. Every parent that an item's own
// coordinates determine is derived here, so identity construction has one
// parent-computation path for all kinds. AutomationPoint and Take are the
// exceptions whose parent (their owning lane or scene) is not among (sequence,
// track, clip): that owner is supplied by construction context via lane_id and is
// otherwise recoverable only from the stored parent_id itself — never re-derive
// it from coordinates. The lane_id parameter carries an AutomationLane for a
// point, a TakeLane for a take, and a Scene for a slot.
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

struct ItemLocation {
    ItemKind kind = ItemKind::Project;
    // Immediate ownership is canonical; the remaining IDs cache ancestor navigation.
    ItemId parent_id;
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    bool active = false;

    constexpr ItemLocation() noexcept = default;
    constexpr ItemLocation(ItemKind item_kind, ItemId parent, ItemId sequence, ItemId track,
                           ItemId clip, bool is_active) noexcept
        : kind(item_kind), parent_id(parent), sequence_id(sequence), track_id(track), clip_id(clip),
          active(is_active) {}

    constexpr bool has_same_owner(const ItemLocation& other) const noexcept {
        return kind == other.kind && parent_id == other.parent_id;
    }
    constexpr auto operator<=>(const ItemLocation&) const = default;
};

enum class IdentityMutationKind : std::uint8_t { Insert, Deactivate, Reactivate };

struct IdentityMutation {
    IdentityMutationKind mutation = IdentityMutationKind::Insert;
    ItemId item;
    ItemLocation location;
};

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

class Project {
  public:
    static runtime::Result<Project, ModelError> create(ProjectInput input);

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    std::uint64_t next_item_id() const noexcept;
    ItemId root_sequence_id() const noexcept;
    std::span<const MediaAsset> assets() const noexcept;
    std::span<const Sequence> sequences() const noexcept;
    const timebase::TempoMap& tempo_map() const noexcept;
    const timebase::MeterMap& meter_map() const noexcept;
    const std::optional<SessionStart>& session_start() const noexcept;
    const MediaAsset* find_asset(ItemId id) const noexcept;
    const Sequence* find_sequence(ItemId id) const noexcept;
    std::optional<ItemLocation> locate(ItemId id) const noexcept;
    std::size_t shared_identity_nodes_with(const Project& other) const;
    bool shares_storage_with(const Project& other) const noexcept;
    /// Process-local identity of the sequence-reference and registered-content
    /// placement shape used by incremental playback invalidation. The token is
    /// preserved across edits that cannot change dependency subscribers and is
    /// replaced before publishing a structurally incompatible snapshot.
    SequenceCompileStructureToken sequence_compile_structure_token() const noexcept;
    static ProjectIdentityStats identity_stats() noexcept;
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

class IdRemapTable {
  public:
    std::span<const std::pair<ItemId, ItemId>> entries() const noexcept {
        return entries_;
    }
    std::optional<ItemId> find(ItemId old_id) const noexcept;

  private:
    friend struct IdRemapBuilder;
    std::vector<std::pair<ItemId, ItemId>> entries_;
};

// Subtree remaps treat MediaRef::asset_id as an external reference. A null
// callback preserves it; a callback can translate it into the destination
// domain. Callback failure leaves the caller's allocator unchanged.
struct ExternalIdFixup {
    void* context = nullptr;
    runtime::Result<ItemId, ModelError> (*map)(void*, ItemId) noexcept = nullptr;

    runtime::Result<ItemId, ModelError> apply(ItemId id) const noexcept;
};

struct RemapIdFixups {
    ExternalIdFixup asset;
    ExternalIdFixup sequence;
};

struct RemappedClip {
    Clip clip;
    IdRemapTable ids;
};
struct RemappedTrack {
    Track track;
    IdRemapTable ids;
};
struct RemappedSequence {
    Sequence sequence;
    IdRemapTable ids;
};

runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    ExternalIdFixup external = {});
runtime::Result<RemappedClip, ModelError> remap_ids(const Clip& clip, ItemIdAllocator& allocator,
                                                    RemapIdFixups fixups);
runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external = {});
runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     RemapIdFixups fixups);
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external = {});
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, RemapIdFixups fixups);
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, std::span<const std::pair<ItemId, ItemId>> carried_ids,
          RemapIdFixups fixups = {});

struct RemappedProject {
    Project project;
    IdRemapTable ids;
};

// Two-pass remap: allocate every owned identity first, then rebuild references.
// `first_id` selects the destination project's monotonic identity domain.
runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id);

} // namespace pulp::timeline
