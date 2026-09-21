#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/runtime/trace.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>

#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/staged_async_trace_ledger.hpp"

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#include "detail/dawn_shared_io_convolution_session.hpp"
#include "detail/shared_io_convolution_session.hpp"
#include <pulp/signal/fft.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace pulp::gpu_audio {

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
#ifndef PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER
#define PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER 0
#endif
#endif

struct GpuConvolver::SharedIoState {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    std::unique_ptr<detail::SharedIoConvolutionSession> session;
    detail::SharedIoConvolutionSession::Callback callback;
    std::vector<float> callback_input;
    std::vector<float> callback_output;
    std::vector<float> normalized_ir_spectrum;
#endif
};

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
namespace {
bool expected_dawn_revision_available() noexcept {
    constexpr std::string_view value = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA;
    return PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER && !value.empty() &&
           value != "unknown";
}
} // namespace
#endif

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response)
    : GpuConvolver(channels, block_size, sample_rate, std::move(impulse_response), kLatencyBlocks) {
}

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response, uint32_t latency_blocks)
    : channels_(channels), block_(block_size), sample_rate_(sample_rate),
      latency_blocks_(latency_blocks), ir_(std::move(impulse_response)) {}

GpuConvolver::~GpuConvolver() = default;

namespace detail {
bool configure_gpu_convolver_trial(GpuConvolver& convolver,
                                   const GpuConvolverTrialConfig& config) noexcept {
    if (convolver.prepared_)
        return false;
    if (config.generation == 0)
        return false;
    convolver.trial_requested_path_ = static_cast<std::uint8_t>(config.requested_path);
    convolver.trial_generation_ = config.generation;
    convolver.trial_configured_ = true;
    convolver.trial_enable_trace_ = config.enable_trace;
    convolver.trial_capture_admissions_ = config.capture_admissions;
    convolver.trial_success_stride_ = std::max<std::uint32_t>(1, config.success_stride);
    convolver.trial_completion_policy_ = static_cast<std::uint8_t>(config.completion_policy);
    convolver.trial_completion_wait_ns_ = config.completion_wait_ns;
    return true;
}

bool drain_gpu_convolver_trial_records(GpuConvolver& convolver,
                                       std::vector<SharedIoTraceRecord>& records) noexcept {
    records.clear();
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    // The staged adapter has no callback-side trace queue. Its worker ledger
    // is the authenticated source of terminal outcomes, and may only be
    // observed once every request has reached a terminal state. Keep this
    // accessor quiescent-only so a producer cannot serialize a partial trial
    // and accidentally treat missing blocks as successful delivery.
    if (convolver.staged_trial_) {
        if (!convolver.staged_trial_->quiescent())
            return false;
        try {
            records = convolver.staged_trial_->take_completed();
            return true;
        } catch (...) {
            records.clear();
            return false;
        }
    }
    if (!convolver.shared_io_ || !convolver.shared_io_->session ||
        !convolver.shared_io_->session->trace_recording_enabled())
        return false;
    try {
        convolver.shared_io_->session->drain_trace_records(
            static_cast<std::uint32_t>(SharedIoTraceRecorder::capacity),
            [&](const SharedIoTraceRecord& record) { records.push_back(record); });
        return true;
    } catch (...) {
        records.clear();
        return false;
    }
#else
    (void)convolver;
    return false;
#endif
}
} // namespace detail

GpuAudioNodeDescriptor GpuConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-convolver";
    d.input_channels = channels_;
    d.output_channels = channels_;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    // kLatencyBlocks of worker headroom for the GPU round-trip, and CpuFallback so
    // a miss is filled by the continuously-fed, latency-aligned CPU convolver —
    // seamless, never a dry glitch. The node has a real CPU fallback, so this is
    // the right default: the GPU contributes when it keeps up, the CPU covers it
    // transparently otherwise, and the plugin always produces correct audio.
    d.latency_blocks = latency_blocks_;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (shared_io_ && shared_io_->session && !shared_io_->session->release())
        return false;
#endif
    shared_io_.reset();
    staged_trial_.reset();
    staged_sequence_ = 0;
    if (channels_ == 0 || block_ == 0 || ir_.empty() || latency_blocks_ == 0 ||
        latency_blocks_ > kMaxLatencyBlocks)
        return false;

    // The CPU fallback is a signal::PartitionedConvolver loaded at `block_`, and
    // load_ir() rounds a non-power-of-two block UP to the next power of two for
    // its radix-2 FFT. The fallback would then be partitioned for a block size
    // the transport never delivers, so every fallback block would be a block-size
    // violation. Refuse to prepare rather than run a convolver whose fallback can
    // only ever fail closed.
    if ((block_ & (block_ - 1u)) != 0u)
        return false;

    // fft_size = next power of two >= block + ir_length (matches signal::Convolver).
    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size()))
        fft_size_ <<= 1;

    const uint32_t cplx = fft_size_ * 2u;
    // Batched across channels: one submit, one readback per block. The IR is
    // mono and resident, so every channel convolves against the same spectrum.
    in_pad_.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
    time_.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
    ir_spec_.assign(cplx, 0.0f);
    carry_.assign(channels_, std::vector<float>(fft_size_, 0.0f));

    // CPU fallback path (RT-safe after load): the continuously-fed RT miss
    // fallback + latency-alignment delay ring, and the no-GPU worker fallback.
    init_fallback();

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    const auto requested_path = static_cast<detail::SharedIoRequest>(trial_requested_path_);
#endif

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (expected_dawn_revision_available() &&
        requested_path != detail::SharedIoRequest::RequireStaged) {
        try {
            constexpr uint32_t kSharedIoCapacity = 8;
            constexpr uint32_t kSharedIoSlots = 2;
            const uint32_t shared_capacity = std::max(kSharedIoCapacity, latency_blocks_ + 1u);
            if (fft_size_ <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                auto state = std::make_unique<SharedIoState>();
                const auto samples_per_block = static_cast<std::size_t>(channels_) * block_;
                state->callback_input.assign(samples_per_block, 0.0f);
                state->callback_output.assign(samples_per_block, 0.0f);

                std::vector<std::complex<float>> spectrum(fft_size_, {0.0f, 0.0f});
                for (uint32_t i = 0; i < ir_.size() && i < fft_size_; ++i)
                    spectrum[i] = {ir_[i], 0.0f};
                signal::FftT<float> fft(static_cast<int>(fft_size_));
                if (fft.ready()) {
                    fft.forward(spectrum.data());
                    state->normalized_ir_spectrum.assign(static_cast<std::size_t>(fft_size_) * 2u,
                                                         0.0f);
                    const float scale = 1.0f / static_cast<float>(fft_size_);
                    for (uint32_t i = 0; i < fft_size_; ++i) {
                        state->normalized_ir_spectrum[2u * i] = spectrum[i].real() * scale;
                        state->normalized_ir_spectrum[2u * i + 1u] = spectrum[i].imag() * scale;
                    }

                    auto created = detail::create_dawn_shared_io_convolution_session(
                        {.provider =
                             {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                              .completion_policy =
                                  trial_configured_
                                      ? static_cast<detail::DawnSharedIoProvider::CompletionPolicy>(
                                            trial_completion_policy_)
                                      : detail::DawnSharedIoProvider::CompletionPolicy::
                                            ProcessEvents,
                              .completion_wait_ns =
                                  trial_configured_ ? trial_completion_wait_ns_ : 0},
                         .session = {.pipeline = {.capacity = shared_capacity,
                                                  .channels = channels_,
                                                  .block_size = block_,
                                                  .fft_size = fft_size_,
                                                  .ir_length = static_cast<uint32_t>(ir_.size()),
                                                  .lead_blocks = latency_blocks_},
                                     .slots = kSharedIoSlots,
                                     .sample_rate = sample_rate_,
                                     .trace = {.success_stride = trial_success_stride_,
                                               .capture_admissions = trial_capture_admissions_,
                                               .enabled = pulp::runtime::kTracingEnabled ||
                                                          trial_enable_trace_}},
                         .normalized_ir_spectrum = state->normalized_ir_spectrum});
                    // A failed preparation may still own physically live
                    // storage. Retain that session even when it cannot run.
                    if (created.session) {
                        state->session = std::move(created.session);
                        shared_io_ = std::move(state);
                    }
                }
            }
        } catch (...) {
            shared_io_.reset();
        }
    }
#endif

    // Shared-I/O owns the live Dawn provider path. Do not also create the legacy
    // standalone GpuCompute device in that mode; offline/staged processing can
    // use the exact CPU worker fallback while realtime uses the shared provider.
    if (!shared_io_) {
        gpu_ = render::GpuCompute::create();
        if (gpu_ && gpu_->initialize_standalone()) {
            std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
            for (uint32_t i = 0; i < ir_.size() && i < fft_size_; ++i) {
                in_pad_[2u * i] = ir_[i]; // real; imag stays 0
            }
            // fft_forward reads only the first block; the rest of in_pad_ is the
            // batch scratch used per process_block.
            if (!gpu_->fft_forward(in_pad_.data(), ir_spec_.data(), fft_size_) ||
                !gpu_->prepare_convolution_batch(fft_size_, ir_spec_.data(), channels_)) {
                gpu_.reset();
            }
        } else {
            gpu_.reset();
        }
    } else {
        gpu_.reset();
    }

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (trial_configured_ && requested_path == detail::SharedIoRequest::RequireStaged && gpu_)
        staged_trial_ = std::make_unique<detail::StagedAsyncTrialState>(2, trial_generation_);
#endif

    prepared_ = true;
    return true;
}

std::uint8_t GpuConvolver::process_realtime_shared_io(void* self,
                                                      const audio::BufferView<const float>& input,
                                                      audio::BufferView<float>& output,
                                                      std::uint32_t n, std::uint64_t sequence,
                                                      bool input_valid) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (convolver == nullptr || !convolver->prepared_ || !convolver->shared_io_ ||
        !convolver->shared_io_->session || !convolver->shared_io_->session->prepared() ||
        n != convolver->block_ || input.num_channels() < convolver->channels_ ||
        output.num_channels() < convolver->channels_ || input.num_samples() < n ||
        output.num_samples() < n)
        return detail::kRealtimeGpuInactive;

    auto& state = *convolver->shared_io_;
    // Invalid external views consume a zero-input timeline position, but may
    // never admit fresh GPU work. The transport owns their silent disposition.
    if (!input_valid)
        state.session->request_recovery(detail::SharedIoRecoveryReason::InvalidCallback);
    for (uint32_t ch = 0; ch < convolver->channels_; ++ch) {
        const float* src = input.channel_ptr(ch);
        auto* dst = state.callback_input.data() + static_cast<std::size_t>(ch) * convolver->block_;
        std::copy_n(src, n, dst);
    }

    const auto callback = state.session->begin_callback(
        std::span<const float>(state.callback_input.data(), state.callback_input.size()), sequence);
    state.callback = callback;
    const auto delivery = state.session->consume_output(
        callback, std::span<float>(state.callback_output.data(), state.callback_output.size()),
        true);

    switch (delivery) {
    case detail::SharedIoConvolutionSession::Delivery::Ready:
        output.clear();
        for (uint32_t ch = 0; ch < convolver->channels_; ++ch) {
            const auto* src =
                state.callback_output.data() + static_cast<std::size_t>(ch) * convolver->block_;
            std::copy_n(src, n, output.channel_ptr(ch));
        }
        return detail::kRealtimeGpuReady;
    case detail::SharedIoConvolutionSession::Delivery::Priming:
        return detail::kRealtimeGpuPriming;
    case detail::SharedIoConvolutionSession::Delivery::Missing:
    case detail::SharedIoConvolutionSession::Delivery::EpochChanged:
    case detail::SharedIoConvolutionSession::Delivery::Invalid:
        return detail::kRealtimeGpuMissed;
    }
#endif
    return detail::kRealtimeGpuInactive;
}

std::uint32_t GpuConvolver::service_realtime_shared_io(void* self, std::uint64_t now_ns) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (convolver == nullptr || !convolver->prepared_ || !convolver->shared_io_ ||
        !convolver->shared_io_->session || !convolver->shared_io_->session->prepared())
        return detail::kRealtimeGpuServiceInactive;
    const auto result = convolver->shared_io_->session->service(now_ns);
    // A configured private trial owns the authenticated trace queue until its
    // quiescent accessor drains it. The normal runtime path may continue to
    // mirror records into Perfetto here, but consuming trial records would
    // make the matched benchmark observe an empty shared path.
    if (!convolver->trial_configured_)
        (void)convolver->shared_io_->session->drain_trace();
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(result.terminal_records,
                              static_cast<std::size_t>(detail::kRealtimeGpuServiceInactive - 1u)));
#else
    (void)self;
    (void)now_ns;
    return detail::kRealtimeGpuServiceInactive;
#endif
}

bool GpuConvolver::fence_realtime_shared_io(void* self) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (convolver == nullptr)
        return true;
    const bool fenced = !convolver->shared_io_ || !convolver->shared_io_->session ||
                        convolver->shared_io_->session->fence_for_offline();
    return fenced;
#else
    (void)self;
    return true;
#endif
}

void GpuConvolver::complete_realtime_shared_io(void* self, std::uint64_t sequence,
                                               std::uint8_t disposition) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (!convolver || !convolver->shared_io_ || !convolver->shared_io_->session)
        return;
    auto& state = *convolver->shared_io_;
    if (state.callback.valid() && state.callback.stamp.sequence == sequence)
        (void)state.session->complete_callback_delivery(
            state.callback, static_cast<detail::SharedIoDeliveryDisposition>(disposition));
#endif
}

std::uint64_t GpuConvolver::next_realtime_shared_io_sequence(void* self) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (convolver && convolver->shared_io_ && convolver->shared_io_->session)
        return convolver->shared_io_->session->next_sequence();
#endif
    return 0;
}

bool GpuConvolver::has_realtime_shared_io() const noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    return prepared_ && shared_io_ && shared_io_->session && shared_io_->session->prepared();
#else
    return false;
#endif
}

void GpuConvolver::process_block(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& output, uint32_t n) {
    if (!prepared_ || n != block_ || input.num_channels() < channels_ ||
        output.num_channels() < channels_) {
        output.clear();
        return;
    }
    if (!gpu_) {
        render_worker_fallback(input, output, n);
        return;
    }

    const uint32_t cplx = fft_size_ * 2u;

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    // Private P4 adapter path. It intentionally waits on the worker thread,
    // preserving the existing blocking process_block contract while attaching
    // one request to an authenticated request/slot/sequence ledger. The public
    // default path never constructs staged_trial_.
    if (staged_trial_) {
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            const float* x = input.channel_ptr(ch);
            float* slot = in_pad_.data() + static_cast<std::size_t>(ch) * cplx;
            for (uint32_t i = 0; i < n; ++i)
                slot[2u * i] = x[i];
        }

        const auto sequence = staged_sequence_++;
        const auto slot = static_cast<std::uint32_t>(sequence % 2u);
        const auto now = std::chrono::steady_clock::now();
        const auto now_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        const auto deadline_ns = std::max<std::uint64_t>(
            1'000'000ull, static_cast<std::uint64_t>(block_) * 1'000'000'000ull / sample_rate_);
        const auto deadline = std::chrono::microseconds(deadline_ns / 1000ull);
        struct Completion {
            bool done = false;
            render::GpuCompute::ReadbackStatus status = render::GpuCompute::ReadbackStatus::Failed;
            std::uint64_t id = 0;
        };
        auto completion = std::make_shared<Completion>();
        const auto request = gpu_->convolve_batch_async(
            in_pad_.data(), time_.data(), fft_size_, channels_, deadline,
            [completion](const render::GpuCompute::ReadbackResult& result) {
                completion->status = result.status;
                completion->id = result.id;
                completion->done = true;
            });
        if (request == 0) {
            output.clear();
            return;
        }
        const bool admitted =
            staged_trial_->admit(request, sequence, slot, now_ns + deadline_ns, now_ns);
        if (!admitted) {
            // GpuCompute has no cancellation API. The callback owns only the
            // heap completion state, so a provider callback that arrives after
            // this failed admission cannot dereference the stack.
            output.clear();
            return;
        }
        if (!staged_trial_->submitted(request, now_ns)) {
            // Admission created an authenticated sequence/slot reservation.
            // If submission cannot attach to that reservation, close it with
            // one cancellation terminal so the producer's ledger remains
            // exactly-once even on this provider-side failure.
            (void)staged_trial_->abandon(request, now_ns);
            output.clear();
            return;
        }
        // GpuCompute guarantees that poll_readbacks() resolves every request at
        // success or at its deadline, so this worker loop is bounded by the
        // provider deadline rather than an unbounded wait on GPU progress.
        while (!completion->done)
            (void)gpu_->poll_readbacks();
        (void)staged_trial_->complete(
            completion->id,
            completion->status == render::GpuCompute::ReadbackStatus::Success
                ? detail::StagedAsyncTraceLedger::CompletionStatus::Success
            : completion->status == render::GpuCompute::ReadbackStatus::Expired
                ? detail::StagedAsyncTraceLedger::CompletionStatus::Expired
                : detail::StagedAsyncTraceLedger::CompletionStatus::Failed,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()));
        if (completion->status != render::GpuCompute::ReadbackStatus::Success) {
            output.clear();
            return;
        }
        for (uint32_t ch = 0; ch < channels_; ++ch)
            detail::overlap_add_block(carry_[ch].data(),
                                      time_.data() + static_cast<std::size_t>(ch) * cplx,
                                      /*src_stride=*/2, output.channel_ptr(ch), fft_size_, n);
        return;
    }
#endif

    // Pack every channel's zero-padded complex block back to back.
    std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        const float* x = input.channel_ptr(ch);
        float* slot = in_pad_.data() + static_cast<std::size_t>(ch) * cplx;
        for (uint32_t i = 0; i < n; ++i)
            slot[2u * i] = x[i];
    }

    // ONE fused, GPU-resident convolution for all channels — forward FFT,
    // complex multiply by the resident IR spectrum, inverse FFT — in a single
    // submit with a single readback. The ~0.5 ms map round trip is paid once
    // per block, not once per channel.
    //
    // On failure emit silence and DO NOT mutate any overlap carry: feeding a
    // stale time_ into the accumulators would poison every subsequent block.
    if (!gpu_->convolve_batch(in_pad_.data(), time_.data(), fft_size_, channels_)) {
        output.clear();
        return;
    }

    for (uint32_t ch = 0; ch < channels_; ++ch) {
        // Guarded overlap-add: add this block's result, emit the first n, shift
        // the carry left by n. A non-finite readback resets the carry and emits
        // silence for this block instead of poisoning the channel forever.
        detail::overlap_add_block(carry_[ch].data(),
                                  time_.data() + static_cast<std::size_t>(ch) * cplx,
                                  /*src_stride=*/2, output.channel_ptr(ch), fft_size_, n);
    }
}

} // namespace pulp::gpu_audio
