#pragma once

#include "shared_io_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pulp::gpu_audio::detail {

class DawnSharedIoProvider final : public SharedIoArenaProvider {
  public:
    enum class Availability : std::uint8_t { Ready, Unsupported, InvalidProvider, Failed };
    enum class Fault : std::uint8_t {
        None,
        RefuseAllocation,
        RefuseInputImport,
        RefuseOutputImport,
        RejectBeforeSubmit,
        PoisonAfterSubmit,
        DelayCompletion,
        WrongOutput,
        PlantWriteBuffer,
        PlantCopyBuffer,
        PlantMapAsync,
        InvalidCommandAfterSubmit,
        SyntheticQueueError,
        SyntheticQueueCancelled,
        ForceLossBeforeSubmit,
        ForceLossBetweenSubmitAndCompletionRegistration,
        ForceLossAfterCompletionRegistration,
        HoldTerminalBusy,
        NativeInputOom,
        NativeOutputOom,
    };

    struct Options {
        std::string expected_dawn_revision;
        // Test-only table prepared before any Dawn object. The provider still
        // authenticates and performs the sole process-global installation.
        const void* proc_table_override_for_testing = nullptr;
        Fault fault = Fault::None;
        std::uint32_t fault_slot = 0;
    };

    struct CreateResult {
        std::unique_ptr<DawnSharedIoProvider> provider;
        Availability availability = Availability::Failed;
        std::string reason;
    };

    struct Stats {
        std::uint64_t slots_created = 0;
        std::uint64_t slots_destroyed = 0;
        std::uint64_t allocations = 0;
        std::uint64_t import_attempts = 0;
        std::uint64_t import_successes = 0;
        std::uint64_t retired_success = 0;
        std::uint64_t retired_failure = 0;
        std::uint64_t disposals_observed = 0;
        std::uint64_t host_frees = 0;
        std::uint64_t drain_calls = 0;
        std::uint64_t failed_drains = 0;
        std::uint64_t terminal_busy_retries = 0;
        std::uint64_t fault_injections = 0;
    };

    struct AdapterIdentity {
        std::string name;
        std::uint32_t vendor_id = 0;
        std::uint32_t device_id = 0;
    };

    static CreateResult create(const Options& options) noexcept;
    ~DawnSharedIoProvider() override;

    DawnSharedIoProvider(const DawnSharedIoProvider&) = delete;
    DawnSharedIoProvider& operator=(const DawnSharedIoProvider&) = delete;

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override;
    void retire_slot(SlotResources& resources) noexcept override;
    void destroy_slot(SlotResources& resources) noexcept override;
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override;
    void poll() noexcept override;
    bool drain() noexcept override;

    std::uint32_t alignment() const noexcept;
    std::uint64_t proc_table_install_count() const noexcept;
    Stats stats() const noexcept;
    AdapterIdentity adapter_identity() const;

  private:
    struct Impl;
    explicit DawnSharedIoProvider(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::gpu_audio::detail
