#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/midi_lane.hpp>
#include <pulp/timeline/note_modifier.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

class SchemaRegistry;

/// Validation and identity failures returned by immutable Timeline model edits.
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
    InvalidMidiLane,
    DuplicateMidiLaneAddress,
    MissingSequenceReference,
    SequenceReferenceCycle,
    SequenceNestingTooDeep,
    InvalidTrackMixer,
    InvalidTimeConform,
    InvalidTuningReference,
    InvalidModulator,
    InvalidMacroControl,
    InvalidModulationRoute,
    MissingModulationSource,
    MissingModulationTarget,
};

/// Model failure with the offending and, when relevant, conflicting identity.
struct ModelError {
    ModelErrorCode code = ModelErrorCode::InvalidItemId;
    ItemId item;
    ItemId related_item;
};

/// Versioned extension-schema identity.
struct SchemaIdentity {
    std::string type_name;
    std::uint32_t version = 0;

    /// Returns whether the type name is non-empty and the version is non-zero.
    bool valid() const noexcept;
    auto operator<=>(const SchemaIdentity&) const = default;
};

/// Monotonic allocator for non-zero document ItemIds.
///
/// Allocation never wraps or reuses an identity; exhaustion is returned as a
/// ModelError and leaves the allocator exhausted.
class ItemIdAllocator {
  public:
    /// Creates an allocator whose next successful result is `next`.
    ///
    /// Zero and `UINT64_MAX` construct an exhausted allocator.
    explicit constexpr ItemIdAllocator(std::uint64_t next = 1) noexcept : next_(next) {}

    /// Returns the next identity and advances the allocator.
    runtime::Result<ItemId, ModelError> allocate() noexcept;
    /// Returns the numeric value that the next successful allocation uses.
    constexpr std::uint64_t next_value() const noexcept {
        return next_;
    }

  private:
    std::uint64_t next_ = 1;
};

/// Clip content that intentionally carries no media, notes, or extension value.
struct EmptyContent {};

/// Project-owned sealed media metadata.
///
/// Frame counts and loop bounds are expressed at `sample_rate`; locators and
/// representations are alternate access paths for the same content hash.
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

/// Borrowed range of frames from a project-owned MediaAsset.
struct MediaRef {
    ItemId asset_id;
    timebase::SamplePosition source_start;
    std::uint64_t frame_count = 0;
};

/// Reference to a sibling sequence in the same Project pool.
///
/// The clip owns neither the sequence nor any of its identities.
struct SequenceRef {
    ItemId sequence_id;
    timebase::TickPosition source_start{0};

    constexpr auto operator<=>(const SequenceRef&) const = default;
};

/// Time domain in which a clip's placement remains anchored.
enum class ClipTimeAnchor : std::uint8_t { Musical, Absolute };

/// Authored policy for adapting a musical media clip when its duration and
/// source duration differ. Non-default intent is invalid for absolute clips
/// and non-media content. Playback support is provided separately.
enum class TimeConform : std::uint8_t {
    None,
    Resample,
    Stretch,
};

/// Musical clip placement in canonical ticks.
struct MusicalTimeRange {
    timebase::TickPosition start;
    timebase::TickDuration duration;
};

/// Absolute clip placement in samples at an explicit rational rate.
struct AbsoluteTimeRange {
    timebase::SamplePosition start;
    std::uint64_t sample_count = 0;
    timebase::RationalRate sample_rate;
};

/// Sequence duration in absolute samples at an explicit rational rate.
struct AbsoluteTimelineDuration {
    std::uint64_t sample_count = 0;
    timebase::RationalRate sample_rate;
};

/// Gain curve traversed across a clip's fade region, in both directions.
///
/// `Linear` is equal-gain: two complementary linear fades over uncorrelated
/// material sum to (1-t)^2 + t^2, which is 0.5 at the midpoint — an audible
/// 3 dB dip. `EqualPower` is the sine quarter-cycle, whose complementary pair
/// squares to sin^2 + cos^2 = 1, holding power constant across the transition.
/// Correlated material (the same take on both sides) inverts the preference,
/// which is why the shape is authored rather than inferred.
enum class ClipFadeShape : std::uint8_t { Linear, EqualPower };

/// Clip-level audio controls. Fade lengths use the clip anchor's native
/// unit: canonical ticks for musical clips and timeline samples for absolute
/// clips. The playback compiler resolves both to sample-exact frame counts.
/// The shape governs both fades; it is a reparameterization of the same
/// normalized progress, so it carries no unit of its own.
struct ClipPlaybackProperties {
    float gain_linear = 1.0f;
    std::uint64_t fade_in_duration = 0;
    std::uint64_t fade_out_duration = 0;
    ClipFadeShape fade_shape = ClipFadeShape::Linear;
    constexpr auto operator<=>(const ClipPlaybackProperties&) const = default;
};

/// Exhaustive authored clip-placement domains.
using ClipTimeRange = std::variant<MusicalTimeRange, AbsoluteTimeRange>;

/// One identity-bearing MIDI-style note in canonical musical ticks.
struct NoteEvent {
    ItemId id;
    timebase::TickPosition start;
    timebase::TickDuration duration;
    std::uint16_t velocity = 0xffff;
    std::uint8_t pitch = 60;
    std::uint8_t channel = 0;
};

/// Immutable MIDI clip content: start-then-identity-ordered notes with sparse
/// deterministic modifiers, beside the controller and expression streams those
/// notes sound against.
///
/// Notes and lanes are separate storage because they answer different
/// questions. A note is asked "which notes start inside this block", which
/// wants events ordered by time across the whole clip. A controller stream is
/// asked "what was your value at this instant", which wants each stream's own
/// history, independently searchable, so that seeking never costs a walk over
/// unrelated streams.
class MidiContent {
  public:
    /// Notes alone: no modifiers, no lanes, and a zero seed. Every note plays
    /// once, unconditionally.
    static runtime::Result<MidiContent, ModelError> create(std::vector<NoteEvent> notes);
    /// Notes plus their sparse modifier companion array. Each modifier must
    /// name a note in `notes`, must be well-formed, and must not be neutral —
    /// a neutral entry is a second encoding of a note that already plays that
    /// way. `modifier_seed` is the document's authored seed for probability
    /// draws; the same seed always reproduces the same decisions.
    static runtime::Result<MidiContent, ModelError> create(std::vector<NoteEvent> notes,
                                                           std::vector<NoteModifier> modifiers,
                                                           std::uint64_t modifier_seed);
    /// Notes, modifiers, seed, and the clip's controller/expression lanes.
    ///
    /// Every lane and point identity must be valid and distinct from every
    /// other identity this content owns, including the note identities, and no
    /// two lanes may share an address. Lanes are returned in `(address, id)`
    /// order and their points in `(position, id)` order regardless of the order
    /// supplied.
    static runtime::Result<MidiContent, ModelError> create(std::vector<NoteEvent> notes,
                                                           std::vector<NoteModifier> modifiers,
                                                           std::uint64_t modifier_seed,
                                                           std::vector<MidiExpressionLane> lanes);
    /// Returns a new snapshot replacing the note with the same identity.
    ///
    /// Modifiers, seed, and every lane carry over untouched: replacing a note
    /// changes what that note plays, never what the clip's controllers say.
    /// Fails if the identity is absent or the replacement note is invalid.
    runtime::Result<MidiContent, ModelError> replace_note(NoteEvent note) const;

    /// Returns notes in canonical `(start, id)` order.
    ///
    /// The span remains valid while this MidiContent snapshot remains alive.
    std::span<const NoteEvent> notes() const noexcept {
        return data_->notes;
    }

    /// Sorted by note id and containing only notes whose playback differs from
    /// the default, so a document that authors no modifiers carries none.
    std::span<const NoteModifier> modifiers() const noexcept {
        return data_->modifiers;
    }

    /// Returns the authored seed used to derive per-note probability draws.
    std::uint64_t modifier_seed() const noexcept {
        return data_->modifier_seed;
    }

    /// Controller and expression streams in `(address, id)` order, each lane's
    /// points in `(position, id)` order.
    ///
    /// Both orderings are what makes a seek cheap: the stream is found by
    /// binary search over this span, and its value at an instant by binary
    /// search within that lane's points. A clip that authors no controllers
    /// carries no lanes.
    std::span<const MidiExpressionLane> lanes() const noexcept {
        return data_->lanes;
    }

    /// The lane addressing `address`, or nullptr when the clip authors none.
    const MidiExpressionLane* lane_for(const MidiLaneAddress& address) const noexcept;

    /// The modifier for `note_id`, or nullptr when the note plays by default.
    const NoteModifier* modifier_for(ItemId note_id) const noexcept;

  private:
    struct Data {
        std::vector<NoteEvent> notes;
        std::vector<NoteModifier> modifiers;
        std::uint64_t modifier_seed = 0;
        std::vector<MidiExpressionLane> lanes;
    };

    explicit MidiContent(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

    std::shared_ptr<const Data> data_;
};

/// Extension-defined typed C++ content admitted by a SchemaRegistry.
///
/// The shared value and canonical JSON are immutable. Registered payloads own
/// no ItemIds, so subtree remapping cannot hide untracked identity references.
class RegisteredContent {
  public:
    /// Returns the schema identity that controls serialization.
    const SchemaIdentity& schema() const noexcept {
        return schema_;
    }
    /// Returns shared ownership of the type-erased immutable value.
    const std::shared_ptr<const void>& value() const noexcept {
        return value_;
    }
    /// Returns the validated canonical JSON representation.
    const std::string& canonical_payload_json() const noexcept {
        return canonical_payload_json_;
    }
    /// Returns the retained-byte charge used for document limits.
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    /// Opaque identity of the immutable schema registry whose codec created
    /// this erased value. Equal type/version text from another registry is not
    /// sufficient to make value_as<T>() safe.
    const std::shared_ptr<const void>& schema_registry_identity() const noexcept {
        return schema_registry_identity_;
    }

    /// Views the registered value as `T`.
    ///
    /// The caller must use the type associated with schema(); no run-time type
    /// check is performed, and the pointer remains owned by this value.
    template <typename T> const T* value_as() const noexcept {
        return static_cast<const T*>(value_.get());
    }

  private:
    friend class SchemaRegistry;
    RegisteredContent(SchemaIdentity schema, std::shared_ptr<const void> value,
                      std::string canonical_payload_json, std::size_t retained_bytes,
                      std::shared_ptr<const void> schema_registry_identity)
        : schema_(std::move(schema)), value_(std::move(value)),
          canonical_payload_json_(std::move(canonical_payload_json)),
          retained_bytes_(retained_bytes),
          schema_registry_identity_(std::move(schema_registry_identity)) {}
    SchemaIdentity schema_;
    std::shared_ptr<const void> value_;
    std::string canonical_payload_json_;
    std::size_t retained_bytes_ = 0;
    std::shared_ptr<const void> schema_registry_identity_;
};

/// Parser bounds retained with an opaque extension envelope.
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

/// Exact validated JSON for an unavailable extension schema.
///
/// Opaque content can be retained and re-saved but cannot be identity-remapped
/// because its internal ownership/reference shape is unknown.
class OpaqueContent {
  public:
    /// Validates `raw_json` within `limits` and stores it without normalization.
    static runtime::Result<OpaqueContent, ModelError>
    create(SchemaIdentity schema, std::string raw_json, OpaqueContentLimits limits = {});

    /// Returns the unavailable schema identity.
    const SchemaIdentity& schema() const noexcept {
        return schema_;
    }
    /// Returns the exact validated JSON bytes supplied at construction.
    const std::string& raw_json() const noexcept {
        return raw_json_;
    }
    /// Returns the bounds under which raw_json() was admitted.
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

/// Exhaustive set of immutable payloads a Clip can own or reference.
using ClipContent = std::variant<EmptyContent, MediaRef, MidiContent, RegisteredContent,
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
///                    [&](const MidiContent& notes) { ... },
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

/// Immutable identity-bearing placement of one ClipContent value.
///
/// Mutation helpers return path-copy snapshots and never alter this value.
class Clip {
  public:
    /// Creates a musical clip in canonical ticks.
    ///
    /// Validates identity, positive duration, content, playback controls, and
    /// that non-default time-conform intent is attached only to media content.
    static runtime::Result<Clip, ModelError> create(ItemId id, timebase::TickPosition start,
                                                    timebase::TickDuration duration,
                                                    ClipContent content,
                                                    ClipPlaybackProperties playback = {},
                                                    TimeConform time_conform = TimeConform::None);
    /// Creates an absolute clip in samples at `sample_rate`.
    ///
    /// Validates identity, positive duration, rate, content, and playback controls.
    /// Absolute clips accept only TimeConform::None.
    static runtime::Result<Clip, ModelError>
    create_absolute(ItemId id, timebase::SamplePosition start, std::uint64_t sample_count,
                    timebase::RationalRate sample_rate, ClipContent content,
                    ClipPlaybackProperties playback = {},
                    TimeConform time_conform = TimeConform::None);

    /// Returns the stable clip identity.
    ItemId id() const noexcept;
    /// Returns the clip's authored time domain.
    ClipTimeAnchor time_anchor() const noexcept;
    /// Returns the complete authored placement range.
    const ClipTimeRange& time_range() const noexcept;
    /// Returns musical start; precondition: time_anchor() is Musical.
    timebase::TickPosition start() const noexcept;
    /// Returns musical duration; precondition: time_anchor() is Musical.
    timebase::TickDuration duration() const noexcept;
    /// Returns musical exclusive end; precondition: time_anchor() is Musical.
    timebase::TickPosition end() const noexcept;
    /// Returns absolute start; precondition: time_anchor() is Absolute.
    timebase::SamplePosition absolute_start() const noexcept;
    /// Returns absolute duration in samples; precondition: anchor is Absolute.
    std::uint64_t absolute_duration_samples() const noexcept;
    /// Returns absolute placement rate; precondition: anchor is Absolute.
    timebase::RationalRate absolute_sample_rate() const noexcept;
    /// Returns absolute exclusive end; precondition: anchor is Absolute.
    timebase::SamplePosition absolute_end() const noexcept;
    /// Returns the immutable content value.
    const ClipContent& content() const noexcept;
    /// Returns a snapshot with validated replacement placement.
    runtime::Result<Clip, ModelError> with_time_range(ClipTimeRange range) const;
    /// Returns a snapshot with validated replacement content.
    runtime::Result<Clip, ModelError> with_content(ClipContent content) const;
    /// Returns a snapshot with validated replacement gain and fades.
    runtime::Result<Clip, ModelError>
    with_playback_properties(ClipPlaybackProperties playback) const;
    /// Returns a snapshot with validated time-conform intent. Non-default intent
    /// requires a musical MediaRef clip.
    runtime::Result<Clip, ModelError> with_time_conform(TimeConform time_conform) const;
    /// Returns the authored gain and fade controls.
    ClipPlaybackProperties playback_properties() const noexcept;
    /// Returns the authored intent. None preserves legacy behavior.
    TimeConform time_conform() const noexcept;

  private:
    struct Data;
    explicit Clip(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

/// @}

} // namespace pulp::timeline
