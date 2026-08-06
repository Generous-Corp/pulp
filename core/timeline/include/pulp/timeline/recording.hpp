#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timeline/clip.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

// A Take is one recorded region that references a sealed media asset. It lives
// in a TakeLane on a Track and is anchored to absolute (sample) time: raw
// captures are sample-accurate against the transport, not musical. The take's
// timeline length is its media frame_count. A Take owns a stable ItemId because
// comps select take segments by identity. The asset reference is
// external (Project::create validates it exists); the take never owns the asset.
/// Immutable sample-anchored recording take referencing a project media asset.
class Take {
  public:
    /// Creates a sample-anchored take referencing a sealed media range.
    ///
    /// Identity, media range, placement, and sample rate are validated.
    static runtime::Result<Take, ModelError> create(ItemId id, MediaRef media,
                                                    timebase::SamplePosition placement_start,
                                                    timebase::RationalRate sample_rate);

    /// Returns the take's stable document identity.
    ItemId id() const noexcept {
        return id_;
    }
    /// Returns the referenced asset range.
    const MediaRef& media() const noexcept {
        return media_;
    }
    /// Returns the absolute timeline start in samples at sample_rate().
    timebase::SamplePosition placement_start() const noexcept {
        return placement_start_;
    }
    /// Returns the rational rate governing placement and take duration.
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

/// Exact absolute-time selection from one take for a comp.
///
/// The range must lie inside the referenced take and use its normalized rate.
struct TakeCompSegment {
    ItemId take_id;
    AbsoluteTimeRange range;
    /// Compares the referenced take and exact absolute-time range.
    constexpr bool operator==(const TakeCompSegment& other) const noexcept {
        return take_id == other.take_id && range.start == other.range.start &&
               range.sample_count == other.range.sample_count &&
               range.sample_rate == other.range.sample_rate;
    }
};

/// Sealed offline-rendered track artifact and its source-plan identity.
struct TrackFreeze {
    MediaRef media;
    timebase::SamplePosition placement_start;
    timebase::RationalRate sample_rate;
    ContentHash render_plan_hash;
    /// Compares the rendered media, placement, rate, and source-plan hash.
    constexpr bool operator==(const TrackFreeze& other) const noexcept {
        return media.asset_id == other.media.asset_id &&
               media.source_start == other.media.source_start &&
               media.frame_count == other.media.frame_count &&
               placement_start == other.placement_start && sample_rate == other.sample_rate &&
               render_plan_hash == other.render_plan_hash;
    }
};

/// Immutable alternate recording lane owned by a Track.
///
/// Takes are canonical by identity. Comp segments are sample-exact,
/// non-overlapping document intent; edits return new snapshots.
class TakeLane {
  public:
    /// Validates and creates a lane with its takes and optional comp.
    static runtime::Result<TakeLane, ModelError> create(ItemId id, std::string name,
                                                        std::vector<Take> takes,
                                                        std::vector<TakeCompSegment> comp = {});
    /// Returns a snapshot with `take` inserted, or a duplicate/invalid error.
    runtime::Result<TakeLane, ModelError> insert_take(Take take) const;
    /// Returns a snapshot without `id`.
    ///
    /// Removal fails when the take is absent or still referenced by the comp.
    runtime::Result<TakeLane, ModelError> erase_take(ItemId id) const;
    /// Returns a snapshot with a validated, canonical replacement comp.
    runtime::Result<TakeLane, ModelError>
    with_comp_segments(std::vector<TakeCompSegment> comp) const;

    /// Returns the lane's stable document identity.
    ItemId id() const noexcept;
    /// Returns the authored lane name.
    const std::string& name() const noexcept;
    /// Returns takes in canonical identity order.
    std::span<const Take> takes() const noexcept;
    /// Returns comp segments ordered by timeline start then take identity.
    std::span<const TakeCompSegment> comp_segments() const noexcept;
    /// Finds a take by identity, or returns `nullptr` when absent.
    const Take* find_take(ItemId id) const noexcept;

  private:
    struct Data;
    explicit TakeLane(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

/// Track-owned linear gain and stereo balance.
///
/// Pan is in [-1, 1]. A centered, unity-gain mixer is the identity operation.
struct TrackMixer {
    float gain_linear = 1.0f;
    float pan = 0.0f;
    constexpr bool operator==(const TrackMixer&) const = default;
};

/// Largest admitted authored linear track gain.
inline constexpr float kMaximumTrackGainLinear = 64.0f;

/// @}

} // namespace pulp::timeline
