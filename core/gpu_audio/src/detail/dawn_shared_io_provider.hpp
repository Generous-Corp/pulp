#pragma once

#include "dawn_shared_io_wavenet_spec.hpp"
#include "shared_io_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::gpu_audio::detail {

class DawnSharedIoConvolutionProgram;

struct SharedIoConvolutionProgramSpec {
    std::uint32_t fft_size = 0;
    std::uint32_t channels = 0;
    std::uint32_t logical_frames = 0;
    std::uint32_t ir_length = 0;
    std::span<const float> normalized_ir_spectrum;
};

class DawnSharedIoProvider final : public SharedIoArenaProvider {
  public:
    enum class Availability : std::uint8_t { Ready, Unsupported, InvalidProvider, Failed };
    // Completion waiting is owned by the serialized provider dispatcher.  The
    // default ProcessEvents policy remains the compatibility path; the wait
    // policies only change how that dispatcher waits for queue futures and do
    // not move any Dawn call onto the audio callback.
    enum class CompletionPolicy : std::uint8_t {
        ProcessEvents,
        WaitAny,
        TimedWaitAny,
    };
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
        SyntheticWaitAnyTimeout,
        SyntheticWaitAnyError,
        ForceLossBeforeSubmit,
        ForceLossBetweenSubmitAndCompletionRegistration,
        ForceLossAfterCompletionRegistration,
        HoldTerminalBusy,
        NativeInputOom,
        NativeOutputOom,
        ConvolutionPrepareAfterScopesFailure,
        ConvolutionSubmitAfterScopesFailure,
    };

    struct Options {
        std::string expected_dawn_revision;
        // Test-only table prepared before any Dawn object. The provider still
        // authenticates and performs the sole process-global installation.
        const void* proc_table_override_for_testing = nullptr;
        Fault fault = Fault::None;
        std::uint32_t fault_slot = 0;
        CompletionPolicy completion_policy = CompletionPolicy::ProcessEvents;
        // Used only by TimedWaitAny.  A zero value uses the provider's
        // dispatcher deadline for each wait; this is not an audio deadline.
        // Each serialized wait is capped at one millisecond.
        // Values above std::chrono::nanoseconds::max().count() fail closed.
        std::uint64_t completion_wait_ns = 0;
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
        std::uint64_t process_events_calls = 0;
        std::uint64_t wait_any_calls = 0;
        std::uint64_t wait_any_timed_calls = 0;
        std::uint64_t wait_any_timeouts = 0;
        std::uint64_t wait_any_errors = 0;
        std::uint64_t wait_any_unsupported = 0;
        std::uint64_t wait_any_max_futures = 0;
        std::uint64_t wait_any_max_timeout_ns = 0;
    };

    struct AdapterIdentity {
        std::string name;
        std::uint32_t vendor_id = 0;
        std::uint32_t device_id = 0;
    };

    static CreateResult create(const Options& options) noexcept;
    std::unique_ptr<SharedIoPreparedProgram>
    make_convolution_program(const SharedIoConvolutionProgramSpec& spec) noexcept;
    // Factory for the authenticated WaveNet preparation boundary. The current
    // private implementation is intentionally mono-only; multi-instance models
    // require an instance-qualified submit token before they can be enabled.
    std::unique_ptr<SharedIoPreparedProgram>
    make_wavenet_program(const DawnSharedIoWavenetProgramSpec& spec) noexcept;
    ~DawnSharedIoProvider() override;

    DawnSharedIoProvider(const DawnSharedIoProvider&) = delete;
    DawnSharedIoProvider& operator=(const DawnSharedIoProvider&) = delete;

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override;
    void retire_slot(SlotResources& resources) noexcept override;
    void destroy_slot(SlotResources& resources) noexcept override;
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override;
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override;
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override;
    void poll() noexcept override;
    bool device_lost() const noexcept override;
    bool drain() noexcept override;

    std::uint32_t alignment() const noexcept;
    std::uint64_t proc_table_install_count() const noexcept;
    Stats stats() const noexcept;
    CompletionPolicy completion_policy() const noexcept;
    AdapterIdentity adapter_identity() const;

    bool prepare_wavenet_program(const DawnSharedIoWavenetProgramSpec& spec,
                                 std::span<const SlotBufferHandle> slots) noexcept;
    bool submit_wavenet_program(const SlotResources&, SlotToken,
                                std::shared_ptr<SharedIoTerminalInbox>) noexcept;
    bool release_wavenet_program() noexcept;

  private:
    friend class DawnSharedIoConvolutionProgram;
    bool prepare_convolution_program(const SharedIoConvolutionProgramSpec&) noexcept;
    bool submit_convolution_program(const SlotResources&, SlotToken,
                                    std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept;
    bool release_convolution_program() noexcept;
    bool submit_impl(const SlotResources&, SlotToken,
                     std::shared_ptr<SharedIoTerminalInbox> terminal_inbox, unsigned kind) noexcept;
    struct Impl;
    explicit DawnSharedIoProvider(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::gpu_audio::detail
