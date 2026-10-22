#include "cpu_shared_io_provider.hpp"

#include "nam_tcn_artifact.hpp"

#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::gpu_audio::detail {

namespace {

// The P1 probe program the Dawn provider runs through its plain submit().
constexpr float kProbeScale = 1.75f;
constexpr float kProbeBias = 0.25f;

// drain() retries a busy inbox cell this many times before it reports an
// unproven barrier. A busy cell means another publisher holds the claim, which
// on this provider can only be its own HoldTerminalBusy injection.
constexpr int kDrainPublishAttempts = 1024;

class ProbeKernel final : public CpuSharedIoArenaProvider::Kernel {
  public:
    bool execute(SharedIoSlotLedger::SlotToken, std::span<const std::byte> input,
                 std::span<std::byte> output) noexcept override {
        const auto floats = std::min(input.size(), output.size()) / sizeof(float);
        for (std::size_t index = 0; index < floats; ++index) {
            float sample = 0.0f;
            std::memcpy(&sample, input.data() + index * sizeof(float), sizeof(float));
            const float mapped = sample * kProbeScale + kProbeBias;
            std::memcpy(output.data() + index * sizeof(float), &mapped, sizeof(float));
        }
        return true;
    }
};

} // namespace

struct CpuSharedIoArenaProvider::Impl {
    struct Submission {
        bool accepted = false;
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        std::optional<SharedIoTerminalStatus> pending;
        bool delayed = false;
        std::optional<SharedIoTerminalInbox::CompletionClaim> held_claim;
    };

    struct Slot {
        std::uint32_t index = 0;
        // float storage keeps every slot float-aligned for the programs.
        std::unique_ptr<float[]> input;
        std::unique_ptr<float[]> output;
        std::size_t input_bytes = 0;
        std::size_t output_bytes = 0;
        std::uint64_t handle_generation = 1;
        bool retired = false;
        Submission submission;
    };

    explicit Impl(const Options& value) : options(value), lifetime(std::make_shared<int>(0)) {}

    Slot* find(const SlotResources& resources) const noexcept {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (slot == nullptr)
            return nullptr;
        for (const auto& owned : slots)
            if (owned.get() == slot)
                return slot;
        return nullptr;
    }

    // One publication attempt for a slot's pending terminal. Service honours a
    // delayed terminal; drain publishes it.
    void publish(Slot& slot, bool draining) noexcept {
        auto& submission = slot.submission;
        if (!submission.accepted || !submission.pending || !submission.inbox)
            return;
        if ((submission.delayed || lost) && !draining)
            return;
        // Work outstanding when the provider was lost retires failed, and
        // only at the drain boundary.
        if (lost)
            submission.pending = SharedIoTerminalStatus::RetiredFailed;
        SharedIoTerminalInbox::PushResult result;
        if (submission.held_claim) {
            // The injected claimant now finishes the publication it held.
            result = submission.inbox->finish_claim(*submission.held_claim, submission.token,
                                                    *submission.pending);
            if (result != SharedIoTerminalInbox::PushResult::Busy)
                submission.held_claim.reset();
        } else if (options.fault == Fault::HoldTerminalBusy && !busy_fault_consumed) {
            // Take the cell's claim first, so the ordinary publication below
            // finds it busy and the terminal must be retained and retried.
            busy_fault_consumed = true;
            auto attempt = submission.inbox->try_claim(submission.token.slot);
            submission.held_claim = attempt.claim;
            result = submission.inbox->push(submission.token, *submission.pending);
        } else {
            result = submission.inbox->push(submission.token, *submission.pending);
        }
        switch (result) {
        case SharedIoTerminalInbox::PushResult::Accepted:
            if (*submission.pending == SharedIoTerminalStatus::RetiredSuccess)
                ++stats.retired_success;
            else
                ++stats.retired_failure;
            submission = {};
            break;
        case SharedIoTerminalInbox::PushResult::Busy:
            ++stats.terminal_busy_retries;
            break;
        case SharedIoTerminalInbox::PushResult::Rejected:
            // An accepted submission owns an exact terminal obligation. A
            // rejection is an invariant failure, not retirement evidence.
            reusable = false;
            break;
        }
    }

    // Loses the provider once, at the armed point, recording when and why.
    void lose(Fault point) noexcept {
        if (lost || options.fault != point)
            return;
        lost = true;
        accepting = false;
        reusable = false;
        stats.device_lost_callback_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
        stats.device_lost_reason = kInjectedLossReason;
    }

    bool any_accepted() const noexcept {
        return std::any_of(slots.begin(), slots.end(),
                           [](const auto& slot) { return slot->submission.accepted; });
    }

    Options options;
    Stats stats;
    std::vector<std::unique_ptr<Slot>> slots;
    std::shared_ptr<int> lifetime;
    bool accepting = true;
    bool reusable = true;
    bool busy_fault_consumed = false;
    bool lost = false;
};

namespace {

class CpuSharedIoConvolutionProgram final : public SharedIoPreparedProgram,
                                            public CpuSharedIoArenaProvider::Kernel {
  public:
    CpuSharedIoConvolutionProgram(CpuSharedIoArenaProvider& provider,
                                  const SharedIoConvolutionProgramSpec& spec)
        : provider_(&provider), fft_size_(spec.fft_size), channels_(spec.channels),
          bytes_(std::size_t(spec.fft_size) * spec.channels * 2u * sizeof(float)),
          ir_(spec.fft_size) {
        for (std::uint32_t bin = 0; bin < spec.fft_size; ++bin)
            ir_[bin] = {spec.normalized_ir_spectrum[2u * bin],
                        spec.normalized_ir_spectrum[2u * bin + 1u]};
    }

    static bool valid(const SharedIoConvolutionProgramSpec& spec) noexcept {
        return spec.fft_size >= 2 && (spec.fft_size & (spec.fft_size - 1)) == 0 &&
               spec.fft_size <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
               spec.channels != 0 && spec.logical_frames != 0 && spec.ir_length != 0 &&
               std::uint64_t(spec.logical_frames) + spec.ir_length - 1u <= spec.fft_size &&
               spec.normalized_ir_spectrum.size() == std::size_t(spec.fft_size) * 2u &&
               std::uint64_t(spec.fft_size) * spec.channels * 2u * sizeof(float) <=
                   std::numeric_limits<std::uint32_t>::max() &&
               // The WOLA variant has no CPU implementation; refuse it rather
               // than run a different program under the same spec.
               spec.spectral_hop == 0 && !spec.spectral_per_hop_gains;
    }

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        if (prepared_ || &provider != provider_ || slots.empty())
            return false;
        for (const auto& handle : slots) {
            std::size_t input_bytes = 0;
            std::size_t output_bytes = 0;
            if (!provider_->validate_slot_buffers(handle) ||
                !provider_->slot_extent(handle, input_bytes, output_bytes) ||
                input_bytes < bytes_ || output_bytes < bytes_)
                return false;
        }
        try {
            fft_ = std::make_unique<signal::FftT<float>>(static_cast<int>(fft_size_));
            scratch_.assign(fft_size_, {});
        } catch (...) {
            fft_.reset();
            return false;
        }
        if (!fft_->ready()) {
            fft_.reset();
            return false;
        }
        prepared_ = true;
        return true;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override {
        return prepared_ && &provider == provider_ &&
               provider_->submit_kernel(resources, token, std::move(terminal_inbox), *this);
    }

    bool release() noexcept override {
        if (provider_ != nullptr && provider_->has_accepted_work())
            return false;
        prepared_ = false;
        fft_.reset();
        scratch_.clear();
        return true;
    }

    // Per channel: forward FFT of the packed complex block, multiply by the
    // normalized IR spectrum, then the unnormalized inverse the Dawn kernel
    // computes (sign +1, no 1/N), written as conj(forward(conj(X))).
    bool execute(SlotToken, std::span<const std::byte> input,
                 std::span<std::byte> output) noexcept override {
        if (!prepared_ || input.size() < bytes_ || output.size() < bytes_)
            return false;
        const auto channel_bytes = std::size_t(fft_size_) * sizeof(std::complex<float>);
        for (std::uint32_t channel = 0; channel < channels_; ++channel) {
            std::memcpy(scratch_.data(), input.data() + channel * channel_bytes, channel_bytes);
            fft_->forward(scratch_.data());
            for (std::uint32_t bin = 0; bin < fft_size_; ++bin)
                scratch_[bin] = std::conj(scratch_[bin] * ir_[bin]);
            fft_->forward(scratch_.data());
            for (auto& value : scratch_)
                value = std::conj(value);
            std::memcpy(output.data() + channel * channel_bytes, scratch_.data(), channel_bytes);
        }
        return true;
    }

  private:
    CpuSharedIoArenaProvider* provider_ = nullptr;
    std::uint32_t fft_size_ = 0;
    std::uint32_t channels_ = 0;
    std::size_t bytes_ = 0;
    std::vector<std::complex<float>> ir_;
    std::vector<std::complex<float>> scratch_;
    std::unique_ptr<signal::FftT<float>> fft_;
    bool prepared_ = false;
};

// Runs the WaveNet program with the existing CPU WaveNet model
// (NamTcnArtifact), the oracle the Dawn WaveNet qualification compares
// against. Its causal history is program state, so submissions must arrive in
// contiguous stream order, exactly as the Dawn program requires.
class CpuSharedIoWavenetProgram final : public SharedIoPreparedProgram,
                                        public CpuSharedIoArenaProvider::Kernel {
  public:
    CpuSharedIoWavenetProgram(CpuSharedIoArenaProvider& provider, std::uint32_t block_size)
        : provider_(&provider), block_size_(block_size) {}

    bool configure(const WavenetProgramSpec& spec) {
        if (!model_.configure_program(spec))
            return false;
        // The artifact defaults to the reference kernel; run the block SIMD
        // engine where it covers the topology, as the neural shadow does.
        model_.set_cpu_kernel(NamCpuKernel::BlockSimd);
        return true;
    }

    NamCpuKernel cpu_kernel() const noexcept {
        return model_.cpu_kernel();
    }

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        if (prepared_ || &provider != provider_ || slots.empty() || !model_.loaded())
            return false;
        const auto bytes = std::size_t(block_size_) * sizeof(float);
        for (const auto& handle : slots) {
            std::size_t input_bytes = 0;
            std::size_t output_bytes = 0;
            if (!provider_->validate_slot_buffers(handle) ||
                !provider_->slot_extent(handle, input_bytes, output_bytes) || input_bytes < bytes ||
                output_bytes < bytes)
                return false;
        }
        model_.reset();
        has_sequence_ = false;
        prepared_ = true;
        return true;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override {
        if (!prepared_ || &provider != provider_)
            return false;
        if (has_sequence_ && (last_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
                              token.stream_sequence != last_sequence_ + 1u))
            return false;
        if (!provider_->submit_kernel(resources, token, std::move(terminal_inbox), *this))
            return false;
        last_sequence_ = token.stream_sequence;
        has_sequence_ = true;
        return true;
    }

    bool release() noexcept override {
        if (provider_ != nullptr && provider_->has_accepted_work())
            return false;
        prepared_ = false;
        return true;
    }

    bool execute(SlotToken, std::span<const std::byte> input,
                 std::span<std::byte> output) noexcept override {
        const auto bytes = std::size_t(block_size_) * sizeof(float);
        if (!prepared_ || input.size() < bytes || output.size() < bytes)
            return false;
        // Slot storage is float-aligned provider memory (see create_slot).
        model_.process(reinterpret_cast<const float*>(input.data()),
                       reinterpret_cast<float*>(output.data()), block_size_);
        return true;
    }

  private:
    CpuSharedIoArenaProvider* provider_ = nullptr;
    std::uint32_t block_size_ = 0;
    NamTcnArtifact model_;
    std::uint64_t last_sequence_ = 0;
    bool has_sequence_ = false;
    bool prepared_ = false;
};

} // namespace

CpuSharedIoArenaProvider::CpuSharedIoArenaProvider() : CpuSharedIoArenaProvider(Options{}) {}

CpuSharedIoArenaProvider::CpuSharedIoArenaProvider(const Options& options)
    : impl_(std::make_unique<Impl>(options)) {}

CpuSharedIoArenaProvider::~CpuSharedIoArenaProvider() = default;

std::unique_ptr<SharedIoPreparedProgram> CpuSharedIoArenaProvider::make_convolution_program(
    const SharedIoConvolutionProgramSpec& spec) noexcept {
    if (!CpuSharedIoConvolutionProgram::valid(spec))
        return {};
    try {
        return std::make_unique<CpuSharedIoConvolutionProgram>(*this, spec);
    } catch (...) {
        return {};
    }
}

std::unique_ptr<SharedIoPreparedProgram>
CpuSharedIoArenaProvider::make_wavenet_program(const WavenetProgramSpec& spec) noexcept {
    // Like the Dawn provider: the submit token carries no stream-instance
    // identity, so multi-instance plans are refused.
    if (spec.stream_instances != 1 || !validate_wavenet_spec(spec).accepted())
        return {};
    try {
        auto program = std::make_unique<CpuSharedIoWavenetProgram>(*this, spec.block_size);
        if (!program->configure(spec))
            return {};
        return program;
    } catch (...) {
        return {};
    }
}

std::optional<NamCpuKernel>
CpuSharedIoArenaProvider::wavenet_cpu_kernel(const SharedIoPreparedProgram& program) noexcept {
    const auto* wavenet = dynamic_cast<const CpuSharedIoWavenetProgram*>(&program);
    if (wavenet == nullptr)
        return std::nullopt;
    return wavenet->cpu_kernel();
}

bool CpuSharedIoArenaProvider::create_slot(std::uint32_t slot_index, std::size_t input_bytes,
                                           std::size_t output_bytes,
                                           SlotResources& resources) noexcept {
    if (!impl_ || !impl_->accepting || input_bytes == 0 || output_bytes == 0 ||
        input_bytes % sizeof(float) != 0 || output_bytes % sizeof(float) != 0)
        return false;
    for (const auto& existing : impl_->slots)
        if (existing->index == slot_index)
            return false;
    auto slot = std::unique_ptr<Impl::Slot>(new (std::nothrow) Impl::Slot);
    if (!slot)
        return false;
    slot->index = slot_index;
    slot->input_bytes = input_bytes;
    slot->output_bytes = output_bytes;
    slot->input.reset(new (std::nothrow) float[input_bytes / sizeof(float)]());
    if (!slot->input)
        return false;
    slot->output.reset(new (std::nothrow) float[output_bytes / sizeof(float)]());
    if (!slot->output)
        return false;
    try {
        impl_->slots.push_back(std::move(slot));
    } catch (...) {
        return false;
    }
    auto& created = *impl_->slots.back();
    // Plain host memory: allocated, never imported into any device.
    resources.input = reinterpret_cast<std::byte*>(created.input.get());
    resources.input_size = input_bytes;
    resources.output = reinterpret_cast<std::byte*>(created.output.get());
    resources.output_size = output_bytes;
    resources.opaque = &created;
    resources.input_lifecycle = {.allocated = true};
    resources.output_lifecycle = {.allocated = true};
    resources.storage_kind = StorageKind::Staged;
    ++impl_->stats.slots_created;
    return true;
}

void CpuSharedIoArenaProvider::retire_slot(SlotResources& resources) noexcept {
    auto* slot = impl_ ? impl_->find(resources) : nullptr;
    if (slot == nullptr || slot->retired)
        return;
    // Host memory has no device handle to dispose; retirement only revokes
    // every buffer capability issued for this slot.
    slot->retired = true;
    ++slot->handle_generation;
}

void CpuSharedIoArenaProvider::destroy_slot(SlotResources& resources) noexcept {
    auto* slot = impl_ ? impl_->find(resources) : nullptr;
    if (slot == nullptr || !slot->retired || slot->submission.accepted)
        return;
    std::erase_if(impl_->slots, [slot](const auto& owned) { return owned.get() == slot; });
    ++impl_->stats.slots_destroyed;
    resources.input = nullptr;
    resources.output = nullptr;
    resources.opaque = nullptr;
    resources.input_lifecycle.host_freed = true;
    resources.output_lifecycle.host_freed = true;
    if (impl_->slots.empty() && impl_->reusable)
        impl_->accepting = true;
}

bool CpuSharedIoArenaProvider::acquire_slot_buffers(const SlotResources& resources,
                                                    SlotBufferHandle& handle) const noexcept {
    auto* slot = impl_ ? impl_->find(resources) : nullptr;
    if (slot == nullptr || slot->retired ||
        resources.input != reinterpret_cast<std::byte*>(slot->input.get()) ||
        resources.output != reinterpret_cast<std::byte*>(slot->output.get()))
        return false;
    handle = {};
    handle.provider = this;
    handle.device = this;
    handle.input_buffer = slot->input.get();
    handle.output_buffer = slot->output.get();
    handle.slot = slot->index;
    handle.generation = slot->handle_generation;
    handle.lifetime = impl_->lifetime;
    return true;
}

bool CpuSharedIoArenaProvider::validate_slot_buffers(
    const SlotBufferHandle& handle) const noexcept {
    if (!impl_ || handle.provider != this || handle.device != this || handle.lifetime.expired() ||
        handle.lifetime.lock() != impl_->lifetime)
        return false;
    return std::any_of(impl_->slots.begin(), impl_->slots.end(), [&](const auto& slot) {
        return slot->index == handle.slot && !slot->retired &&
               handle.input_buffer == slot->input.get() &&
               handle.output_buffer == slot->output.get() &&
               handle.generation == slot->handle_generation;
    });
}

bool CpuSharedIoArenaProvider::slot_extent(const SlotBufferHandle& handle, std::size_t& input_bytes,
                                           std::size_t& output_bytes) const noexcept {
    if (!validate_slot_buffers(handle))
        return false;
    for (const auto& slot : impl_->slots) {
        if (slot->index == handle.slot) {
            input_bytes = slot->input_bytes;
            output_bytes = slot->output_bytes;
            return true;
        }
    }
    return false;
}

bool CpuSharedIoArenaProvider::submit(
    const SlotResources& resources, SlotToken token,
    std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept {
    ProbeKernel probe;
    return submit_kernel(resources, token, std::move(terminal_inbox), probe);
}

bool CpuSharedIoArenaProvider::submit_kernel(const SlotResources& resources, SlotToken token,
                                             std::shared_ptr<SharedIoTerminalInbox> terminal_inbox,
                                             Kernel& kernel) noexcept {
    auto* slot = impl_ ? impl_->find(resources) : nullptr;
    if (slot != nullptr)
        impl_->lose(Fault::ForceLossBeforeSubmit);
    const bool refused = slot == nullptr || !impl_->accepting || !terminal_inbox || slot->retired ||
                         slot->submission.accepted || token.slot != slot->index ||
                         resources.input != reinterpret_cast<std::byte*>(slot->input.get()) ||
                         resources.output != reinterpret_cast<std::byte*>(slot->output.get()) ||
                         resources.input_size != slot->input_bytes ||
                         resources.output_size != slot->output_bytes ||
                         (impl_->options.fault == Fault::RejectBeforeSubmit &&
                          impl_->options.fault_slot == slot->index);
    if (refused) {
        if (impl_)
            ++impl_->stats.refused_submissions;
        return false;
    }
    // The work runs here, on the serialized dispatcher, before submit returns.
    const bool executed = kernel.execute(token, {resources.input, resources.input_size},
                                         {resources.output, resources.output_size}) &&
                          impl_->options.fault != Fault::FailExecution;
    impl_->lose(Fault::ForceLossBetweenSubmitAndCompletionRegistration);
    auto& submission = slot->submission;
    submission.accepted = true;
    submission.token = token;
    submission.inbox = std::move(terminal_inbox);
    submission.pending =
        executed ? SharedIoTerminalStatus::RetiredSuccess : SharedIoTerminalStatus::RetiredFailed;
    submission.delayed =
        impl_->options.fault == Fault::DelayCompletion && impl_->options.fault_slot == slot->index;
    ++impl_->stats.submissions;
    impl_->lose(Fault::ForceLossAfterCompletionRegistration);
    return true;
}

void CpuSharedIoArenaProvider::poll() noexcept {
    if (!impl_)
        return;
    for (auto& slot : impl_->slots)
        impl_->publish(*slot, false);
}

void CpuSharedIoArenaProvider::service_until(std::uint64_t) noexcept {
    // Work has already run; nothing is ever waited for.
    poll();
}

bool CpuSharedIoArenaProvider::drain() noexcept {
    if (!impl_)
        return true;
    impl_->accepting = false;
    ++impl_->stats.drain_calls;
    for (int attempt = 0; attempt < kDrainPublishAttempts && impl_->any_accepted(); ++attempt)
        for (auto& slot : impl_->slots)
            impl_->publish(*slot, true);
    if (impl_->any_accepted()) {
        // A terminal the inbox would not take is not retirement evidence.
        ++impl_->stats.failed_drains;
        impl_->reusable = false;
        return false;
    }
    // A clean drain is the provider's resume boundary, as for Dawn.
    if (impl_->reusable)
        impl_->accepting = true;
    return true;
}

bool CpuSharedIoArenaProvider::device_lost() const noexcept {
    return impl_ && impl_->lost;
}

bool CpuSharedIoArenaProvider::can_resume_after_drain() const noexcept {
    return impl_ && !impl_->lost && impl_->reusable && impl_->accepting && !impl_->any_accepted();
}

bool CpuSharedIoArenaProvider::has_accepted_work() const noexcept {
    return impl_ && impl_->any_accepted();
}

SharedIoProviderIdentity CpuSharedIoArenaProvider::provider_identity() const noexcept {
    SharedIoProviderIdentity identity;
    try {
        identity.provider = GpuAudioProvider::Cpu;
        identity.provider_revision = kProviderRevision;
        identity.adapter_name = kAdapterName;
        identity.adapter_backend = kAdapterBackend;
        // The provider is its own executor: there is no separate adapter or
        // native graphics runtime whose identity could be observed.
        identity.authenticated = true;
    } catch (...) {
        return {};
    }
    return identity;
}

SharedIoProviderCapabilities CpuSharedIoArenaProvider::provider_capabilities() const noexcept {
    SharedIoProviderCapabilities capabilities;
    // Host memory that is never imported, and no GPU clock to timestamp.
    capabilities.imported_host_pointer = false;
    capabilities.gpu_timestamps = false;
    // Submissions execute serially in submit order on the dispatcher, and
    // terminals publish through the ordinary completion service.
    capabilities.ordered_causal_state = true;
    capabilities.completion_service = true;
    // There is no device to lose, so no loss recovery is claimed.
    capabilities.device_loss_recovery = false;
    return capabilities;
}

CpuSharedIoArenaProvider::Stats CpuSharedIoArenaProvider::stats() const noexcept {
    return impl_ ? impl_->stats : Stats{};
}

CpuSharedIoConvolutionSessionCreateResult create_cpu_shared_io_convolution_session(
    const CpuSharedIoConvolutionSessionOptions& options) noexcept {
    CpuSharedIoConvolutionSessionCreateResult result;
    try {
        auto provider = std::make_unique<CpuSharedIoArenaProvider>(options.provider);
        const auto& config = options.session.pipeline;
        auto program = provider->make_convolution_program(
            {.fft_size = config.fft_size,
             .channels = config.channels,
             .logical_frames = config.block_size,
             .ir_length = config.ir_length,
             .normalized_ir_spectrum = options.normalized_ir_spectrum});
        if (!program) {
            result.reason =
                CpuSharedIoConvolutionSessionCreateResult::Reason::ProgramConstructionFailed;
            return result;
        }
        auto session_config = options.session;
        session_config.storage_kind = SharedIoArenaProvider::StorageKind::Staged;
        auto session = std::make_unique<SharedIoConvolutionSession>();
        if (!session->prepare({std::move(provider), std::move(program)}, session_config)) {
            result.session = std::move(session);
            result.reason =
                CpuSharedIoConvolutionSessionCreateResult::Reason::SessionPreparationFailed;
            return result;
        }
        result.session = std::move(session);
        result.reason = CpuSharedIoConvolutionSessionCreateResult::Reason::Ready;
    } catch (...) {
        result.reason = CpuSharedIoConvolutionSessionCreateResult::Reason::ConstructionException;
    }
    return result;
}

} // namespace pulp::gpu_audio::detail
