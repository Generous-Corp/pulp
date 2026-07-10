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
/// The push-based adapters (**CLAP, VST3**) get a fixed prepared maximum from
/// the host at activate/setup time and handle an overrun the same, defensible
/// way: **clamp the processed region to that maximum and zero-fill the
/// un-processable tail `[prepared_max, requested)` on each main output
/// channel**, so the host reads clean silence past the prepared max rather than
/// uninitialised memory or a DSP state corrupted by a scratch overrun. They
/// share `clamp_block_to_prepared_max` below.
///
/// Two adapters intentionally differ because their host ABI does:
///  - **AAX** has no host-declared maximum, so it *grows* its scratch to fit
///    (off the real-time thread — only offline AudioSuite can exceed the
///    real-time ceiling) rather than silencing a legitimately larger block. It
///    still calls the helper as a defensive backstop, but it is a no-op once
///    the scratch has grown.
///  - **AU v3** renders through a *pull* graph (`pullInputBlock`), so clamping
///    would advance this node fewer frames than the host and desync the pull
///    chain. It rejects an overrun with `kAudioUnitErr_TooManyFramesToProcess`
///    (the spec response); `maximumFramesToRender` is a hard host contract that
///    is never exceeded in practice.
///
/// This header owns the clamp decision (the pure, testable part); each clamping
/// adapter still zero-fills its own host-buffer layout (f32/f64, planar/
/// interleaved) inline right after it resolves its output pointers.

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
