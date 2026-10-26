#pragma once

// SharedIoConvolutionProgramSpec lives in the Dawn provider header, which
// declares no Dawn type and so compiles on every platform.
#include "dawn_shared_io_provider.hpp"
#include "shared_io_arena.hpp"
#include "shared_io_convolution_session.hpp"
#include "shared_io_wavenet_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace pulp::gpu_audio::detail {

enum class NamCpuKernel : std::uint8_t; // nam_tcn_artifact.hpp

// Host-memory implementation of the shared-I/O provider seam. It owns plain
// heap slots and runs each accepted submission's program on the serialized
// dispatcher, inside submit(), with Pulp's existing CPU kernels. The terminal
// for that submission is published from poll()/service_until() or, at the
// latest, by drain(), so the terminal contract is the Dawn provider's: one
// terminal per accepted submission, none for a refusal, and nothing after
// drain() returns. It never touches a GPU and is compiled on every platform.
//
// Slots are Staged storage: the backing is never imported into any device,
// so it cannot satisfy an ImportedHostPointer request.
class CpuSharedIoArenaProvider final : public SharedIoArenaProvider {
  public:
    // Test-only faults that mirror the Dawn provider's injections of the same
    // names. Each targets `fault_slot`.
    enum class Fault : std::uint8_t {
        None,
        // Refuse the submission before any work runs.
        RejectBeforeSubmit,
        // Run the work but withhold its terminal from service until drain().
        DelayCompletion,
        // The first terminal publication finds its inbox cell busy.
        HoldTerminalBusy,
        // Run the work, then retire it RetiredFailed.
        FailExecution,
        // Injected device loss, for the conformance kit's loss scenarios. The
        // first submission loses the provider at the named point: before its
        // work runs (the submission is refused), after its work runs but
        // before its terminal is recorded, or after the terminal is recorded.
        // Once lost, the provider refuses submissions and new slots, holds
        // every outstanding terminal until drain() retires it RetiredFailed,
        // reports device_lost(), and never resumes. Production never sets it.
        ForceLossBeforeSubmit,
        ForceLossBetweenSubmitAndCompletionRegistration,
        ForceLossAfterCompletionRegistration,
    };

    struct Options {
        Fault fault = Fault::None;
        std::uint32_t fault_slot = 0;
    };

    struct Stats {
        std::uint64_t slots_created = 0;
        std::uint64_t slots_destroyed = 0;
        std::uint64_t submissions = 0;
        std::uint64_t refused_submissions = 0;
        std::uint64_t retired_success = 0;
        std::uint64_t retired_failure = 0;
        std::uint64_t terminal_busy_retries = 0;
        std::uint64_t drain_calls = 0;
        std::uint64_t failed_drains = 0;
        // Recorded when an injected loss occurs: steady-clock time and a
        // nonzero reason. Zero means the provider has not been lost.
        std::uint64_t device_lost_callback_ns = 0;
        std::uint32_t device_lost_reason = 0;
    };

    // Reason recorded for an injected loss.
    static constexpr std::uint32_t kInjectedLossReason = 1;

    // Executes one prepared program over a slot's host bytes. Returning false
    // retires the accepted submission RetiredFailed.
    class Kernel {
      public:
        virtual ~Kernel() = default;
        virtual bool execute(SlotToken token, std::span<const std::byte> input,
                             std::span<std::byte> output) noexcept = 0;
    };

    static constexpr const char* kProviderRevision = "pulp-cpu-provider-1";
    static constexpr const char* kAdapterName = "Pulp CPU provider";
    static constexpr const char* kAdapterBackend = "cpu";

    // Construction allocates the provider state and may throw bad_alloc.
    CpuSharedIoArenaProvider();
    explicit CpuSharedIoArenaProvider(const Options& options);
    ~CpuSharedIoArenaProvider() override;

    CpuSharedIoArenaProvider(const CpuSharedIoArenaProvider&) = delete;
    CpuSharedIoArenaProvider& operator=(const CpuSharedIoArenaProvider&) = delete;

    // Mirrors DawnSharedIoProvider::make_convolution_program: forward FFT of
    // the packed complex input, multiply by the normalized IR spectrum, then an
    // unnormalized inverse FFT, per channel. The spectral-hop (WOLA) variant
    // is not implemented on the CPU provider and returns null.
    std::unique_ptr<SharedIoPreparedProgram>
    make_convolution_program(const SharedIoConvolutionProgramSpec& spec) noexcept;
    // Mirrors DawnSharedIoProvider::make_wavenet_program: one mono stream,
    // causal history resident in the program, contiguous submit sequences.
    std::unique_ptr<SharedIoPreparedProgram>
    make_wavenet_program(const WavenetProgramSpec& spec) noexcept;
    // Diagnostic: the CPU kernel a WaveNet program from this provider runs
    // (block SIMD where the engine covers the topology, else the reference);
    // nullopt for any other program.
    static std::optional<NamCpuKernel>
    wavenet_cpu_kernel(const SharedIoPreparedProgram& program) noexcept;

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override;
    void retire_slot(SlotResources& resources) noexcept override;
    void destroy_slot(SlotResources& resources) noexcept override;
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override;
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override;
    // Without a program this runs the P1 probe the Dawn provider runs:
    // output[i] = input[i] * 1.75 + 0.25 over the slot's floats.
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override;
    void poll() noexcept override;
    void service_until(std::uint64_t deadline_ns) noexcept override;
    // Only an injected loss fault can lose this provider.
    bool device_lost() const noexcept override;
    bool can_resume_after_drain() const noexcept override;
    SharedIoProviderIdentity provider_identity() const noexcept override;
    SharedIoProviderCapabilities provider_capabilities() const noexcept override;
    bool drain() noexcept override;

    // Program entry point: validates the slot exactly as submit() does, runs
    // `kernel` over the slot's bytes, and records the pending terminal.
    bool submit_kernel(const SlotResources& resources, SlotToken token,
                       std::shared_ptr<SharedIoTerminalInbox> terminal_inbox,
                       Kernel& kernel) noexcept;
    // Logical byte sizes of a live slot named by a validated handle.
    bool slot_extent(const SlotBufferHandle& handle, std::size_t& input_bytes,
                     std::size_t& output_bytes) const noexcept;
    // True while any accepted submission has not yet published its terminal.
    bool has_accepted_work() const noexcept;

    Stats stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CpuSharedIoConvolutionSessionOptions {
    CpuSharedIoArenaProvider::Options provider;
    // storage_kind is forced to Staged: the CPU provider never imports.
    SharedIoConvolutionSession::Config session;
    std::span<const float> normalized_ir_spectrum;
};

struct CpuSharedIoConvolutionSessionCreateResult {
    enum class Reason : std::uint8_t {
        Ready,
        ProgramConstructionFailed,
        SessionPreparationFailed,
        ConstructionException,
    };

    // On SessionPreparationFailed this remains non-null so the caller owns
    // retrying release(), matching the Dawn factory's ownership transaction.
    std::unique_ptr<SharedIoConvolutionSession> session;
    Reason reason = Reason::ConstructionException;
};

// Non-RT construction seam with the shape of
// create_dawn_shared_io_convolution_session(), backed by the CPU provider.
CpuSharedIoConvolutionSessionCreateResult
create_cpu_shared_io_convolution_session(const CpuSharedIoConvolutionSessionOptions&) noexcept;

} // namespace pulp::gpu_audio::detail
