#pragma once

// Deterministic shared-I/O arena provider for health tests. It never completes
// work; tests flip `lost` to stand in for the backend's loss query.

#include "detail/gpu_convolver_test_access.hpp"
#include "detail/shared_io_convolution_session.hpp"

#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::test {

using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoPreparedProgram;

class LossProvider final : public SharedIoArenaProvider {
  public:
    bool create_slot(std::uint32_t index, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        try {
            auto slot = std::make_unique<Slot>();
            slot->index = index;
            slot->input.resize(input_bytes);
            slot->output.resize(output_bytes);
            resources.input = slot->input.data();
            resources.input_size = input_bytes;
            resources.output = slot->output.data();
            resources.output_size = output_bytes;
            resources.opaque = slot.get();
            resources.input_lifecycle = {
                .allocated = true, .import_attempted = true, .import_succeeded = true};
            resources.output_lifecycle = resources.input_lifecycle;
            slots_.push_back(std::move(slot));
            return true;
        } catch (...) {
            return false;
        }
    }
    void retire_slot(SlotResources& resources) noexcept override {
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }
    void destroy_slot(SlotResources& resources) noexcept override {
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        const auto* slot = static_cast<const Slot*>(resources.opaque);
        if (!slot)
            return false;
        handle = {.provider = this,
                  .device = this,
                  .input_buffer = slot->input.data(),
                  .output_buffer = slot->output.data(),
                  .slot = slot->index,
                  .generation = 1,
                  .lifetime = lifetime_};
        return true;
    }
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        return handle.provider == this && !handle.lifetime.expired();
    }
    bool submit(const SlotResources&, SlotToken,
                std::shared_ptr<pulp::gpu_audio::detail::SharedIoTerminalInbox>) noexcept override {
        return false;
    }
    // Stands in for the Dawn shared-I/O provider in the convolver tests, so it
    // reports that identity the way the real provider reports its own.
    gpu_audio::detail::SharedIoProviderIdentity provider_identity() const noexcept override {
        gpu_audio::detail::SharedIoProviderIdentity identity;
        identity.provider = gpu_audio::GpuAudioProvider::Dawn;
        identity.authenticated = true;
        return identity;
    }
    bool device_lost() const noexcept override {
        return lost;
    }
    void poll() noexcept override {}
    bool drain() noexcept override {
        return true;
    }

    bool lost = false;

  private:
    struct Slot {
        std::uint32_t index = 0;
        std::vector<std::byte> input;
        std::vector<std::byte> output;
    };
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<std::unique_ptr<Slot>> slots_;
};

class IdleProgram final : public SharedIoPreparedProgram {
  public:
    bool prepare(SharedIoArenaProvider&,
                 std::span<const SharedIoArenaProvider::SlotBufferHandle>) noexcept override {
        return true;
    }
    bool submit(SharedIoArenaProvider&, const SharedIoArenaProvider::SlotResources&,
                SharedIoArenaProvider::SlotToken,
                std::shared_ptr<pulp::gpu_audio::detail::SharedIoTerminalInbox>) noexcept override {
        return false;
    }
    bool release() noexcept override {
        return true;
    }
};

// Replace a prepared convolver's provider with a loss-provider session whose
// geometry matches the convolver. False when shared-I/O state is not compiled.
inline bool install_loss_session(pulp::gpu_audio::GpuConvolver& convolver, std::uint32_t block,
                                 LossProvider*& provider) noexcept {
    try {
        auto owner = std::make_unique<LossProvider>();
        provider = owner.get();
        auto session = std::make_unique<pulp::gpu_audio::detail::SharedIoConvolutionSession>();
        if (!session->prepare(
                {std::move(owner), std::make_unique<IdleProgram>()},
                {.pipeline = {.capacity = 8,
                              .channels = 1,
                              .block_size = block,
                              .fft_size = block,
                              .ir_length = 1,
                              .lead_blocks = pulp::gpu_audio::GpuConvolver::kLatencyBlocks},
                 .slots = 2}))
            return false;
        return pulp::gpu_audio::detail::GpuConvolverTestAccess::install_shared_io_session(
            convolver, std::move(session));
    } catch (...) {
        return false;
    }
}

} // namespace pulp::test
