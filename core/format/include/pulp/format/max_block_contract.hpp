#pragma once

/// @file max_block_contract.hpp
/// The one max-block-overrun contract shared by every format adapter.
///
/// A Processor and all of its scratch buffers are sized once, off the audio
/// thread, to a prepared maximum block size (CLAP `clap_activate`, VST3
/// `setupProcessing`, AU v3 `allocateRenderResources`, AAX's first prepare).
/// A well-behaved host never renders more frames than that maximum, so the
/// only thing this contract governs is the pathological overrun.
///
/// Every adapter handles an overrun the same, defensible way: **clamp the
/// processed region to the prepared maximum and zero-fill the un-processable
/// tail `[prepared_max, requested)` on each main output channel**, so the host
/// reads clean silence past the prepared max rather than uninitialised memory
/// or a DSP state corrupted by a scratch overrun. No adapter rejects the block
/// (AU v3 used to) and none re-prepares on the render thread (AAX used to);
/// re-preparing allocates, and rejecting drops the block outright.
///
/// This header owns the clamp decision (the pure, testable part); each adapter
/// still zero-fills its own host-buffer layout (f32/f64, planar/interleaved)
/// inline right after it resolves its output pointers.

namespace pulp::format {

/// Clamp a requested per-block frame count to the prepared maximum.
///
/// Returns @p requested unchanged when it fits within @p prepared_max (the
/// overwhelmingly common path), or when @p prepared_max is unset (<= 0, i.e.
/// "unbounded" — the adapter never established a max). Otherwise returns
/// @p prepared_max, and the caller must zero-fill the tail
/// `[prepared_max, requested)` on every main output channel.
///
/// Pure + constexpr so the contract is unit-tested directly, without standing
/// up an adapter or a host SDK.
constexpr int clamp_block_to_prepared_max(int requested, int prepared_max) noexcept {
    return (prepared_max > 0 && requested > prepared_max) ? prepared_max : requested;
}

}  // namespace pulp::format
