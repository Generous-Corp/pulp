#pragma once

#include "dawn_shared_io_wavenet_spec.hpp"
#include "shared_io_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

class DawnSharedIoProvider;

// Private preparation boundary for the authenticated Dawn WaveNet adapter.
// Dawn objects remain provider-owned; this object only carries the immutable
// shape and delegates preparation/submission to that provider.
class DawnSharedIoWavenetProgram final : public SharedIoPreparedProgram {
  public:
    static std::unique_ptr<DawnSharedIoWavenetProgram>
    create(DawnSharedIoProvider& provider, const DawnSharedIoWavenetProgramSpec& spec);
    static std::unique_ptr<DawnSharedIoWavenetProgram>
    create(const DawnSharedIoWavenetProgramSpec& spec);

    ~DawnSharedIoWavenetProgram() override = default;
    DawnSharedIoWavenetProgram(const DawnSharedIoWavenetProgram&) = delete;
    DawnSharedIoWavenetProgram& operator=(const DawnSharedIoWavenetProgram&) = delete;

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override;
    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override;
    bool release() noexcept override;

    bool prepared() const noexcept {
        return prepared_;
    }
    std::size_t history_bytes() const noexcept {
        return history_.size();
    }
    std::size_t weight_count() const noexcept {
        return weights_.size();
    }

  private:
    DawnSharedIoWavenetProgram(DawnSharedIoProvider* provider,
                               const DawnSharedIoWavenetProgramSpec& spec);

    DawnSharedIoProvider* provider_ = nullptr;
    std::uint32_t block_size_ = 0;
    std::uint32_t stream_instances_ = 0;
    std::vector<float> weights_;
    std::vector<std::byte> history_;
    std::vector<DawnSharedIoWavenetLayerSpec> arrays_;
    std::vector<std::vector<std::uint32_t>> dilations_;
    float head_scale_ = 1.0f;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
