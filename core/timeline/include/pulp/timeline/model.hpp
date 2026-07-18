#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
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

struct ItemId {
    std::uint64_t value = 0;

    // Zero is the null sentinel and UINT64_MAX is the exhausted allocator state.
    constexpr bool valid() const noexcept {
        return value != 0 && value != std::numeric_limits<std::uint64_t>::max();
    }
    constexpr auto operator<=>(const ItemId&) const = default;
};

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
};

struct ModelError {
    ModelErrorCode code = ModelErrorCode::InvalidItemId;
    ItemId item;
    ItemId related_item;
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

using ClipContent = std::variant<EmptyContent, MediaRef, NoteContent>;

class Clip {
  public:
    static runtime::Result<Clip, ModelError> create(ItemId id, timebase::TickPosition start,
                                                    timebase::TickDuration duration,
                                                    ClipContent content);
    static runtime::Result<Clip, ModelError>
    create_absolute(ItemId id, timebase::SamplePosition start, std::uint64_t sample_count,
                    timebase::RationalRate sample_rate, ClipContent content);

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
};

enum class ItemKind : std::uint8_t { Project, Asset, Sequence, Track, Clip, Note };

struct ItemLocation {
    ItemKind kind = ItemKind::Project;
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    bool active = false;
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

template <> struct std::hash<pulp::timeline::ItemId> {
    std::size_t operator()(pulp::timeline::ItemId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
