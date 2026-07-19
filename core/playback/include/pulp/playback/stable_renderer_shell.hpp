#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/runtime/seqlock.hpp>

#include <cstdint>
#include <utility>

namespace pulp::playback {

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
    /// Non-owning block view. `program` must be kept alive by an enclosing
    /// immutable audio-thread publication for this object's whole lifetime.
    explicit PlaybackProgramBlock(const PlaybackProgram* program) noexcept
        : borrowed_(program) {}
    const PlaybackProgram* program() const noexcept {
        return borrowed_ != nullptr ? borrowed_ : guard_.get();
    }
    explicit operator bool() const noexcept { return program() != nullptr; }

  private:
    friend class PlaybackProgramBlockLatch;
    explicit PlaybackProgramBlock(PlaybackProgramStore::ReadGuard guard) noexcept
        : guard_(std::move(guard)) {}
    PlaybackProgramStore::ReadGuard guard_;
    const PlaybackProgram* borrowed_ = nullptr;
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
    void reset() noexcept;
    RendererCarryState state_snapshot() const noexcept { return state_.read(); }
    RendererProgramKey active_key() const noexcept { return state_.read().key; }

  private:
    timeline::ItemId track_id_;
    RendererProgramKey active_key_;
    ProviderKind active_provider_ = ProviderKind::Arrangement;
    runtime::SeqLock<RendererCarryState> state_;
};

} // namespace pulp::playback
