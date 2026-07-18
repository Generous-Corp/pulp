#include <pulp/playback/stable_renderer_shell.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>

namespace pulp::playback {

PlaybackProgramBlock PlaybackProgramBlockLatch::begin_block(
    const PlaybackProgramStore& store) const noexcept {
    runtime::ScopedNoAlloc no_alloc;
    return PlaybackProgramBlock(store.read());
}

StableRendererBlockView StableRendererShell::begin_block(
    const PlaybackProgramBlock& block) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!block) return {};
    const auto* candidate = block.program()->find_track(track_id_);
    if (!candidate) return {};
    if (!candidate->provider().available(candidate->provider().selected))
        return {nullptr, ShellAdoptionResult::Rejected};
    const RendererProgramKey next{candidate->id(), candidate->generation()};
    if (next == active_key_) return {candidate, ShellAdoptionResult::Unchanged};
    if (!is_monotonic_renderer_adoption(active_key_, next))
        return {nullptr, ShellAdoptionResult::Rejected};

    auto carry = state_.read();
    const bool may_carry = active_key_.generation != 0 &&
        candidate->state_policy() == RendererStatePolicy::CarryByItemId &&
        carry.valid && carry.key == active_key_ && carry.active_provider == active_provider_ &&
        candidate->provider().selected == active_provider_;
    if (active_key_.generation != 0 &&
        candidate->state_policy() == RendererStatePolicy::CarryByItemId && !may_carry)
        return {nullptr, ShellAdoptionResult::Rejected};
    if (!may_carry) carry = {};
    carry.key = next;
    carry.active_provider = candidate->provider().selected;
    carry.valid = true;
    state_.write(carry);
    active_key_ = next;
    active_provider_ = candidate->provider().selected;
    return {candidate, ShellAdoptionResult::Adopted};
}

bool StableRendererShell::end_block(RendererCarryState state) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (active_key_.generation == 0 || !state.valid || state.key != active_key_ ||
        state.active_provider != active_provider_) return false;
    state_.write(state);
    return true;
}

} // namespace pulp::playback
