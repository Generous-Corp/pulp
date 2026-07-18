#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/runtime/seqlock.hpp>

#include <cstdint>
#include <compare>
#include <utility>

namespace pulp::playback {

struct RendererProgramKey {
    timeline::ItemId item_id;
    ProgramGeneration generation = 0;
    constexpr auto operator<=>(const RendererProgramKey&) const = default;
};

constexpr bool is_monotonic_renderer_adoption(RendererProgramKey active,
                                               RendererProgramKey candidate) noexcept {
    return candidate.item_id.valid() && candidate.generation != 0 &&
        (active.generation == 0 ||
         (candidate.item_id == active.item_id && candidate.generation > active.generation));
}

struct RendererCarryState {
    RendererProgramKey key;
    ProviderKind active_provider = ProviderKind::Arrangement;
    bool valid = false;
    std::int64_t event_cursor = 0;
    timebase::SamplePosition source_sample;
    timebase::TickPosition timeline_tick;
    std::uint64_t loop_iteration = 0;
};

class PlaybackProgramBlock {
  public:
    PlaybackProgramBlock(PlaybackProgramBlock&&) noexcept = default;
    PlaybackProgramBlock& operator=(PlaybackProgramBlock&&) = delete;
    PlaybackProgramBlock(const PlaybackProgramBlock&) = delete;
    PlaybackProgramBlock& operator=(const PlaybackProgramBlock&) = delete;
    const PlaybackProgram* program() const noexcept { return guard_.get(); }
    explicit operator bool() const noexcept { return program() != nullptr; }

  private:
    friend class PlaybackProgramBlockLatch;
    explicit PlaybackProgramBlock(PlaybackProgramStore::ReadGuard guard) noexcept
        : guard_(std::move(guard)) {}
    PlaybackProgramStore::ReadGuard guard_;
};

class PlaybackProgramBlockLatch {
  public:
    static constexpr audio::RtSafetyClass begin_block_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;
    [[nodiscard]] PlaybackProgramBlock begin_block(const PlaybackProgramStore& store) const noexcept;
};

enum class ShellAdoptionResult : std::uint8_t { Adopted, Unchanged, Missing, Rejected };

struct StableRendererBlockView {
    const TrackProgram* program = nullptr;
    ShellAdoptionResult adoption = ShellAdoptionResult::Missing;
};

class StableRendererShell {
  public:
    static constexpr audio::RtSafetyClass begin_block_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
    static constexpr audio::RtSafetyClass end_block_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    explicit StableRendererShell(timeline::ItemId track_id) noexcept : track_id_(track_id) {}
    StableRendererBlockView begin_block(const PlaybackProgramBlock& block) noexcept;
    bool end_block(RendererCarryState state) noexcept;
    RendererCarryState state_snapshot() const noexcept { return state_.read(); }
    RendererProgramKey active_key() const noexcept { return state_.read().key; }

  private:
    timeline::ItemId track_id_;
    RendererProgramKey active_key_;
    ProviderKind active_provider_ = ProviderKind::Arrangement;
    runtime::SeqLock<RendererCarryState> state_;
};

} // namespace pulp::playback
