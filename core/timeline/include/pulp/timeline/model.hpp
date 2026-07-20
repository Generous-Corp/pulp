#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/device_placement.hpp>
#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline {

namespace detail {
class ProjectStateAccess;
}
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
};

struct MediaRef {
    ItemId asset_id;
    timebase::SamplePosition source_start;
    std::uint64_t frame_count = 0;
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
    static runtime::Result<NoteContent, ModelError> create(std::vector<NoteEvent> notes);
    runtime::Result<NoteContent, ModelError> replace_note(NoteEvent note) const;

    std::span<const NoteEvent> notes() const noexcept {
        return *notes_;
    }

  private:
    explicit NoteContent(std::shared_ptr<const std::vector<NoteEvent>> notes)
        : notes_(std::move(notes)) {}

    std::shared_ptr<const std::vector<NoteEvent>> notes_;
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

using ClipContent =
    std::variant<EmptyContent, MediaRef, NoteContent, RegisteredContent, OpaqueContent>;

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

struct TrackInput {
    ItemId id;
    std::string name;
    std::vector<Clip> clips;
    std::vector<DevicePlacement> device_chain;
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

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    // Ordered by (anchor, start, id) over a persistent AVL timeline index.
    ClipView clips() const noexcept;
    // Binary-searches the persistent ID index. Returned storage is snapshot-owned.
    const Clip* find_clip(ItemId id) const noexcept;
    // Preserves authored processing order. Returned storage is snapshot-owned.
    std::span<const DevicePlacement> device_chain() const noexcept;
    const DevicePlacement* find_device_placement(ItemId id) const noexcept;
    std::size_t shared_index_nodes_with(const Track& other) const;
    bool shares_storage_with(const Track& other) const noexcept;
    static TrackIndexStats index_stats() noexcept;

  private:
    struct Data;
    explicit Track(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

class Sequence {
  public:
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> duration,
           std::vector<Track> tracks);
    static runtime::Result<Sequence, ModelError>
    create(ItemId id, std::string name, std::optional<timebase::TickDuration> musical_duration,
           std::optional<AbsoluteTimelineDuration> absolute_duration, std::vector<Track> tracks);

    ItemId id() const noexcept;
    const std::string& name() const noexcept;
    std::optional<timebase::TickDuration> duration() const noexcept;
    std::optional<AbsoluteTimelineDuration> absolute_duration() const noexcept;
    std::span<const Track> tracks() const noexcept;
    const Track* find_track(ItemId id) const noexcept;
    runtime::Result<Sequence, ModelError> replace_track(Track track) const;
    bool shares_storage_with(const Sequence& other) const noexcept;

  private:
    struct Data;
    explicit Sequence(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
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
};

enum class ItemKind : std::uint8_t {
    Project,
    Asset,
    Sequence,
    Track,
    Clip,
    Note,
    DevicePlacement,
};

constexpr ItemId immediate_parent_id(ItemKind kind, ItemId project_id, ItemId sequence_id,
                                     ItemId track_id, ItemId clip_id) noexcept {
    switch (kind) {
    case ItemKind::Project:
        return {};
    case ItemKind::Asset:
    case ItemKind::Sequence:
        return project_id;
    case ItemKind::Track:
        return sequence_id;
    case ItemKind::Clip:
    case ItemKind::DevicePlacement:
        return track_id;
    case ItemKind::Note:
        return clip_id;
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
    const MediaAsset* find_asset(ItemId id) const noexcept;
    const Sequence* find_sequence(ItemId id) const noexcept;
    std::optional<ItemLocation> locate(ItemId id) const noexcept;
    std::size_t shared_identity_nodes_with(const Project& other) const;
    bool shares_storage_with(const Project& other) const noexcept;
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
runtime::Result<RemappedTrack, ModelError> remap_ids(const Track& track, ItemIdAllocator& allocator,
                                                     ExternalIdFixup external = {});
runtime::Result<RemappedSequence, ModelError>
remap_ids(const Sequence& sequence, ItemIdAllocator& allocator, ExternalIdFixup external = {});

struct RemappedProject {
    Project project;
    IdRemapTable ids;
};

// Two-pass remap: allocate every owned identity first, then rebuild references.
// `first_id` selects the destination project's monotonic identity domain.
runtime::Result<RemappedProject, ModelError> remap_ids(const Project& project,
                                                       std::uint64_t first_id);

} // namespace pulp::timeline
