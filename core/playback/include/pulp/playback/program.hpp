#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/runtime/slot.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <atomic>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pulp::playback {

class ProgramCompilerTask;

using ProgramGeneration = std::uint64_t;

enum class ProgramErrorCode : std::uint8_t {
    InvalidGeneration,
    InvalidIdentity,
    DuplicateTrack,
    DuplicateClip,
    MissingTempoMap,
};

struct ProgramError {
    ProgramErrorCode code = ProgramErrorCode::InvalidGeneration;
    timeline::ItemId item;
};

runtime::Result<ProgramGeneration, ProgramError>
next_program_generation(ProgramGeneration current) noexcept;

enum class ProviderKind : std::uint8_t { Arrangement, Launcher, ExternalInput };

struct ProviderSelectorProgram {
    ProviderKind selected = ProviderKind::Arrangement;
    std::uint8_t available_mask = 1u;

    constexpr bool available(ProviderKind provider) const noexcept {
        const auto index = static_cast<unsigned>(provider);
        return index <= static_cast<unsigned>(ProviderKind::ExternalInput) &&
            (available_mask & (1u << index)) != 0;
    }
};

enum class RendererStatePolicy : std::uint8_t { Stateless, CarryByItemId };

class TrackProgram {
  public:
    timeline::ItemId id() const noexcept { return id_; }
    ProgramGeneration generation() const noexcept { return generation_; }
    ProviderSelectorProgram provider() const noexcept { return provider_; }
    RendererStatePolicy state_policy() const noexcept { return state_policy_; }
    std::span<const timeline::ItemId> ordered_clip_ids() const noexcept { return clip_ids_; }

  private:
    friend class ProgramCompilerTask;
    TrackProgram(timeline::ItemId id, ProgramGeneration generation,
                 ProviderSelectorProgram provider, RendererStatePolicy state_policy,
                 std::vector<timeline::ItemId> clip_ids) noexcept;

    timeline::ItemId id_;
    ProgramGeneration generation_ = 0;
    ProviderSelectorProgram provider_;
    RendererStatePolicy state_policy_ = RendererStatePolicy::CarryByItemId;
    std::vector<timeline::ItemId> clip_ids_;
};

class PlaybackProgram {
  public:
    ProgramGeneration generation() const noexcept { return generation_; }
    std::uint64_t document_revision() const noexcept { return document_revision_; }
    timeline::ItemId project_id() const noexcept { return project_id_; }
    timeline::ItemId sequence_id() const noexcept { return sequence_id_; }
    const timebase::CompiledTempoMap& tempo_map() const noexcept { return *tempo_map_; }
    const std::shared_ptr<const timebase::CompiledTempoMap>& tempo_map_owner() const noexcept {
        return tempo_map_;
    }
    std::span<const std::shared_ptr<const TrackProgram>> tracks() const noexcept {
        return tracks_;
    }
    const TrackProgram* find_track(timeline::ItemId id) const noexcept;

  private:
    friend class ProgramCompilerTask;
    PlaybackProgram(ProgramGeneration generation, std::uint64_t document_revision,
                    timeline::ItemId project_id, timeline::ItemId sequence_id,
                    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                    std::vector<std::shared_ptr<const TrackProgram>> tracks) noexcept;
    const std::shared_ptr<const TrackProgram>*
    find_track_owner(timeline::ItemId id) const noexcept;

    ProgramGeneration generation_ = 0;
    std::uint64_t document_revision_ = 0;
    timeline::ItemId project_id_;
    timeline::ItemId sequence_id_;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    std::vector<std::shared_ptr<const TrackProgram>> tracks_;
};

class PlaybackProgramStore {
  public:
    using ReadGuard = runtime::Slot<const PlaybackProgram>::ReadGuard;
    [[nodiscard]] ReadGuard read() const noexcept { return slot_.read(); }
    bool has_value() const noexcept { return slot_.has_value(); }

  private:
    friend struct PlaybackProgramCompilerCore;
    friend class ProgramCompilerTask;
    void publish(std::shared_ptr<const PlaybackProgram> program) { slot_.publish(std::move(program)); }
    const std::shared_ptr<const PlaybackProgram>& live() const noexcept { return slot_.live(); }
    bool try_bind_compiler() noexcept {
        bool expected = false;
        return compiler_bound_.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel);
    }
    void unbind_compiler() noexcept { compiler_bound_.store(false, std::memory_order_release); }
    runtime::Slot<const PlaybackProgram> slot_;
    std::atomic<bool> compiler_bound_{false};
};

} // namespace pulp::playback
