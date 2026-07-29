#pragma once

#include <pulp/playback/tempo_sync.hpp>

#include <cstddef>
#include <memory>

namespace pulp::playback {

/// Desktop Ableton Link adapter. This Pulp-owned declaration contains no Link
/// types and is exposed only by the opt-in, source-build-only
/// `pulp::ableton-link` target.
class AbletonLinkTempoSync final : public TempoSyncSource {
  public:
    explicit AbletonLinkTempoSync(double initial_tempo_bpm);
    ~AbletonLinkTempoSync() override;

    AbletonLinkTempoSync(const AbletonLinkTempoSync&) = delete;
    AbletonLinkTempoSync& operator=(const AbletonLinkTempoSync&) = delete;
    AbletonLinkTempoSync(AbletonLinkTempoSync&&) = delete;
    AbletonLinkTempoSync& operator=(AbletonLinkTempoSync&&) = delete;

    /// Link-specific controls. These intentionally do not belong to the
    /// backend-neutral TempoSyncSource interface.
    void set_enabled(bool enabled);
    bool enabled() const;
    void set_start_stop_sync_enabled(bool enabled);
    bool start_stop_sync_enabled() const;
    std::size_t peer_count() const;
    TempoSyncError capture_audio_block(const TempoSyncBlockRequest& request,
                                       TempoSyncBlockState& state) noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::playback
