#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <pulp/gpu_audio/gpu_wavenet.hpp>

namespace pulp::gpu_audio {

namespace detail {
struct GpuAudioProviderPoolTestAccess;
class WaveNetRealtimeChannel;
} // namespace detail

/// Host-owned cache of prepared GPU-audio providers that survive a node's
/// release, so the next prepare() of a node with the same program skips
/// device, pipeline and resident-weight creation.
///
/// A node opts in through its config (for example
/// `GpuWaveNetRealtimeNode::Config::provider_pool`). On release the node
/// drains its provider and, when the provider can resume, returns it here
/// instead of destroying it; a later prepare() with the same program key
/// (model shape and weights, block size, slots and completion policy) borrows
/// it back. A borrowed provider's causal history is stale, so the node
/// re-fills it before delivering. Entries older than `keep_warm_ns` are
/// destroyed by evict_expired(), which the host calls from a timer or any
/// non-realtime thread. The audio callback never touches the pool.
///
/// Only a provider that drained cleanly, can resume, and has not lost its
/// device is kept; anything else is released instead. A provider is lent only
/// to a request for the same program on the same provider kind and revision.
/// At capacity the least recently returned provider is evicted.
///
/// Thread safety: every member is safe from any host thread, and the
/// transport's non-realtime worker takes the same mutex when an automatic
/// re-acquire borrows a provider. The pool must outlive every node configured
/// with it.
class GpuAudioProviderPool {
  public:
    struct Config {
        /// How long a returned provider stays warm before eviction.
        std::uint64_t keep_warm_ns = 30'000'000'000;
        /// Warm providers kept at once; returning one more evicts the least
        /// recently returned.
        std::uint32_t capacity = 4;
        /// Budget for releasing each evicted provider. One that cannot drain
        /// in time has its device destroyed and is kept quarantined for the
        /// life of the process rather than freed while the GPU may touch it.
        std::uint64_t release_budget_ns = 250'000'000;
    };
    struct Stats {
        std::uint64_t hits = 0;         // prepare() borrowed a warm provider
        std::uint64_t misses = 0;       // prepare() found no matching provider
        std::uint64_t returned = 0;     // release() kept a provider warm
        std::uint64_t evictions = 0;    // providers destroyed by age, capacity or clear()
        std::uint32_t warm_entries = 0; // providers held now
    };

    GpuAudioProviderPool();
    explicit GpuAudioProviderPool(const Config& config);
    /// Destroys every warm provider.
    ~GpuAudioProviderPool();
    GpuAudioProviderPool(const GpuAudioProviderPool&) = delete;
    GpuAudioProviderPool& operator=(const GpuAudioProviderPool&) = delete;
    GpuAudioProviderPool(GpuAudioProviderPool&&) = delete;
    GpuAudioProviderPool& operator=(GpuAudioProviderPool&&) = delete;

    /// Destroys providers held longer than `keep_warm_ns`.
    void evict_expired() noexcept;
    /// Destroys every warm provider.
    void clear() noexcept;
    Stats stats() const noexcept;

  private:
    friend class GpuWaveNetRealtimeNode;
    friend struct detail::GpuAudioProviderPoolTestAccess;
    // Node-facing. A null result is a miss; the caller creates a provider.
    // `provider_key` names the provider kind and revision; providers found
    // lost on the way are released within `release_budget_ns` in total (zero:
    // Config::release_budget_ns).
    std::unique_ptr<detail::WaveNetRealtimeChannel>
    acquire(const GpuWaveNetSession::Config&, const std::string& provider_key,
            std::uint64_t release_budget_ns) noexcept;
    // Takes a drained, resumable channel; false hands it back to the caller.
    bool give(const GpuWaveNetSession::Config&, const std::string& provider_key,
              std::unique_ptr<detail::WaveNetRealtimeChannel>&) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::gpu_audio
