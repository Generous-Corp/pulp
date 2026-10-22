#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/runtime/trace.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>

#include "detail/gpu_convolver_test_access.hpp"
#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_convolution_session.hpp"
#include "detail/staged_async_trace_ledger.hpp"
#include "detail/worker_pipeline_hosting.hpp"

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#include "detail/dawn_shared_io_convolution_session.hpp"
#include <pulp/signal/fft.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace pulp::gpu_audio {

bool GpuConvolver::gpu_available() const {
    return gpu_ != nullptr || has_realtime_shared_io();
}

std::string GpuConvolver::backend() const {
    return gpu_                       ? gpu_->capabilities().backend
           : has_realtime_shared_io() ? std::string("Metal")
                                      : std::string();
}

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
#ifndef PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER
#define PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER 0
#endif
#endif

struct GpuConvolver::StagedReadbackState {
    // The transport's worker-pipelining opt-in; detached by ~GpuConvolver.
    std::shared_ptr<detail::WorkerPipelineLink> pipeline_link;

    // Settings made before prepare(); survive re-preparation.
    GpuConvolverStagedBreaker breaker{};

    // Latched recovery cause (GpuAudioRecoveryReason), read by status readers
    // on other threads.
    std::atomic<std::uint8_t> reason{0};
    // Worker call (block) index at which `reason` was latched. Written before
    // the reason is published; only the worker writes either.
    std::atomic<std::uint64_t> reason_sequence{UINT64_MAX};

    // Blocks the worker emits behind its submit (descriptor's
    // worker_pipeline_blocks): block N is submitted on call N and judged on
    // call N + pipeline_blocks. 0 means the block is decided inside its call.
    std::uint32_t pipeline_blocks = 0;
    // Worker calls so far; atomic because status readers report it.
    std::atomic<std::uint64_t> calls{0};

    // One slot per block in flight (pipeline_blocks + 1), reused in rotation.
    // The completion callback finds its slot by index and accepts the result
    // only if the slot still waits on that request id; a slot judged before
    // its result landed has id 0, so a late completion is discarded.
    struct Slot {
        std::uint64_t id = 0;
        bool attempted = false;
        bool done = false;
        render::GpuCompute::ReadbackStatus status = render::GpuCompute::ReadbackStatus::Failed;
        std::vector<float> result; // [channels][2 * fft_size] readback
        std::vector<float> input;  // [channels][block] the block's input
        std::vector<const float*> input_ptrs;
        // When the request was submitted: its result is due pipeline_blocks
        // periods later, whatever the call rate.
        std::chrono::steady_clock::time_point submitted{};
    };
    // Slots only ever grow: an abandoned request may still complete into a
    // slot's buffers after the depth changes, so none is freed before the
    // device is. `slot_count` is the rotation in use.
    std::vector<Slot> slots;
    std::size_t slot_count = 1;

    // Set by GpuAudioTransport while it hosts the node (the private hosting
    // hook below). Only a hosted node pipelines: a direct process_block()
    // caller keeps block-in, block-out semantics.
    bool transport_hosted = false;

    // Blocks the CPU convolution still owes output for: after the last block
    // routed to it, the IR tail of that block spans this many more blocks.
    std::uint32_t cpu_tail_blocks = 0;
    std::uint32_t cpu_tail_length = 0; // ceil((ir_length - 1) / block)
    std::uint64_t cpu_blocks = 0;      // diagnostic: blocks the CPU ran
    std::uint64_t gpu_blocks = 0;      // diagnostic: blocks whose GPU result was used
    std::uint64_t breaker_trips = 0;   // diagnostic

    // Breaker. `tripped` means CPU mode with periodic probes; `probation`
    // counts on-time results after a successful probe.
    std::uint32_t consecutive_misses = 0;
    bool tripped = false;
    // Mirror of `tripped` for status readers on other threads.
    std::atomic<bool> cpu_mode{false};
    std::uint32_t probe_countdown = 0;
    std::uint32_t probation_hits = 0;
    bool on_probation = false;

    // One-block-lead fallback only: the worker's CPU-convolution cost (EWMA,
    // ns), reserved out of the in-block wait so a miss still fits in a period.
    std::uint64_t cpu_block_ns = 0;
    std::uint64_t pending_id = 0;
    bool done = false;
    render::GpuCompute::ReadbackStatus status = render::GpuCompute::ReadbackStatus::Failed;

    std::vector<float> zeros;   // one channel block of silence (CPU tail feed)
    std::vector<float> cpu_out; // [channels][block] CPU convolution output
    std::vector<const float*> zero_ptrs;
    std::vector<float*> cpu_out_ptrs;
};

struct GpuConvolver::SharedIoState {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    std::optional<detail::GpuConvolverTrialConfig> trial_config;
    std::unique_ptr<detail::SharedIoConvolutionSession> session;
    detail::SharedIoConvolutionSession::Callback callback;
    std::vector<float> callback_input;
    std::vector<float> callback_output;
    std::vector<float> normalized_ir_spectrum;
    // Latest serialized health sample, readable from host/UI threads.
    detail::RealtimeGpuHealthMirror health;
#endif
};

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
namespace {
bool expected_dawn_revision_available() noexcept {
    constexpr std::string_view value = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA;
    return PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER && !value.empty() &&
           value != "unknown";
}

// Bounded completion wait for each shared-path service step: a quarter block,
// at most 1 ms. The worker can then pick up a completion already in flight
// within the same pump instead of on its next wake.
std::uint64_t completion_service_wait_ns(std::uint32_t block, std::uint32_t sample_rate) noexcept {
    if (block == 0 || sample_rate == 0)
        return 0;
    const auto quarter_block_ns = std::uint64_t(block) * 250'000'000ull / sample_rate;
    return std::min<std::uint64_t>(quarter_block_ns, 1'000'000ull);
}
} // namespace
#endif

namespace {
// Bounds of the staged readback wait. The provider's deadline is one block
// period minus the worker's CPU-convolution cost and a margin, never below the
// floor; the worker gives up a fixed slack after that deadline.
constexpr std::uint64_t kStagedWaitFloorNs = 100'000;
constexpr std::uint64_t kStagedWaitMarginNs = 200'000;
constexpr std::uint64_t kStagedGiveUpSlackNs = 100'000;

constexpr std::uint8_t kTrialPathMask = 0x07;
constexpr std::uint8_t kTrialSlotsShift = 3;
constexpr std::uint64_t kTrialWaitMask = UINT64_C(0xffffffff);
constexpr unsigned kTrialRetentionShift = 32;

std::uint8_t encode_trial_path_and_slots(detail::SharedIoRequest path,
                                         std::uint32_t slots) noexcept {
    std::uint8_t slot_code = 0;
    switch (slots) {
    case 2:
        slot_code = 1;
        break;
    case 4:
        slot_code = 2;
        break;
    case 8:
        slot_code = 3;
        break;
    case 16:
        slot_code = 4;
        break;
    default:
        break;
    }
    return static_cast<std::uint8_t>(path) |
           static_cast<std::uint8_t>(slot_code << kTrialSlotsShift);
}

std::uint32_t decode_trial_slots(std::uint8_t encoded) noexcept {
    switch (encoded >> kTrialSlotsShift) {
    case 1:
        return 2;
    case 2:
        return 4;
    case 3:
        return 8;
    case 4:
        return 16;
    default:
        return 0;
    }
}
} // namespace

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response)
    : GpuConvolver(channels, block_size, sample_rate, std::move(impulse_response), kLatencyBlocks) {
}

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response, uint32_t latency_blocks)
    : channels_(channels), block_(block_size), sample_rate_(sample_rate),
      latency_blocks_(latency_blocks), ir_(std::move(impulse_response)),
      staged_readback_(std::make_unique<StagedReadbackState>()) {
    staged_readback_->pipeline_link = std::make_shared<detail::WorkerPipelineLink>(
        this, &GpuConvolver::set_staged_pipeline_hosting);
}

GpuConvolver::~GpuConvolver() {
    // A transport released after this node reaches only the detached link.
    staged_readback_->pipeline_link->detach();
    // Destroying the device completes any readback the worker abandoned, and
    // that completion reads the staged state; release the device first.
    gpu_.reset();
}

bool GpuConvolver::set_provider_policy(ProviderPolicy policy) noexcept {
    if (prepared_)
        return false;
    provider_policy_ = policy;
    return true;
}

bool GpuConvolver::configure_staged_breaker(const GpuConvolverStagedBreaker& config) noexcept {
    if (prepared_ || config.trip_after_misses == 0 || config.probe_interval_blocks == 0 ||
        config.recover_after_hits == 0)
        return false;
    staged_readback_->breaker = config;
    return true;
}

GpuConvolverStagedBreaker GpuConvolver::staged_breaker() const noexcept {
    return staged_readback_->breaker;
}

GpuAudioRecoveryReason GpuConvolver::staged_latched_reason() const noexcept {
    return static_cast<GpuAudioRecoveryReason>(
        staged_readback_->reason.load(std::memory_order_acquire));
}

bool GpuConvolver::configure_trace(const GpuConvolverTraceConfig& config) noexcept {
    if (prepared_ || config.success_stride == 0)
        return false;
    trial_enable_trace_ = config.enabled;
    trial_capture_admissions_ = config.capture_admissions;
    trial_capture_callback_timing_ = config.capture_callback_timing;
    trial_success_stride_ = config.success_stride;
    return true;
}

namespace detail {
bool configure_gpu_convolver_trial(GpuConvolver& convolver,
                                   const GpuConvolverTrialConfig& config) noexcept {
    if (convolver.prepared_)
        return false;
    if (config.generation == 0)
        return false;
    if (config.slots != 0 && config.slots != 2 && config.slots != 4 && config.slots != 8 &&
        config.slots != 16)
        return false;
    // The private packed wait field preserves the exported object layout. The
    // provider contract caps waits well below this boundary; reject rather
    // than silently truncating an out-of-range diagnostic value.
    if (config.completion_wait_ns > kTrialWaitMask)
        return false;

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    // Stage every potentially-throwing operation before changing the
    // convolver's packed trial fields.  In particular, an allocation failure
    // while creating SharedIoState or copying its trial config must leave a
    // previously configured convolver untouched; otherwise a false return
    // would still arm a partially configured diagnostic on the next prepare.
    std::optional<GpuConvolverTrialConfig> staged_trial_config;
    std::unique_ptr<GpuConvolver::SharedIoState> staged_shared_io;
    try {
        staged_trial_config = config;
        if (!convolver.shared_io_) {
            staged_shared_io = std::make_unique<GpuConvolver::SharedIoState>();
            staged_shared_io->trial_config = std::move(staged_trial_config);
        }
    } catch (...) {
        return false;
    }

    if (staged_shared_io) {
        convolver.shared_io_ = std::move(staged_shared_io);
    } else {
        // Swapping an already constructed optional is allocation-free and
        // preserves the old config until all staging above has succeeded.
        convolver.shared_io_->trial_config.swap(staged_trial_config);
    }
#endif

    // Preserve the public class layout while carrying this private probe-only
    // capacity alongside the existing private path selector.
    convolver.trial_requested_path_ =
        encode_trial_path_and_slots(config.requested_path, config.slots);
    convolver.trial_generation_ = config.generation;
    convolver.trial_configured_ = true;
    convolver.trial_enable_trace_ = config.enable_trace;
    convolver.trial_capture_admissions_ = config.capture_admissions;
    convolver.trial_capture_callback_timing_ = config.capture_callback_timing;
    convolver.trial_staged_sync_reference_ = config.staged_sync_reference;
    convolver.trial_success_stride_ = std::max<std::uint32_t>(1, config.success_stride);
    convolver.trial_completion_policy_ = static_cast<std::uint8_t>(config.completion_policy);
    // Pack the two private diagnostic values into the pre-existing field so
    // the exported GpuConvolver object layout remains unchanged.
    convolver.trial_completion_wait_ns_ =
        (config.completion_wait_ns & kTrialWaitMask) |
        (static_cast<std::uint64_t>(config.retention_capacity) << kTrialRetentionShift);
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
    if (convolver.trial_staged_sync_reference_ && !convolver.staged_sync_records_.empty()) {
        try {
            records = convolver.staged_sync_records_;
            return true;
        } catch (...) {
            records.clear();
            return false;
        }
    }
    if (!convolver.shared_io_ || !convolver.shared_io_->session)
        return false;
    try {
        if (convolver.shared_io_->session->prepared() && !convolver.shared_io_->session->release())
            return false;
        if (convolver.shared_io_->session->last_closed_trace_retention_overflow())
            return false;
        records = convolver.shared_io_->session->take_last_closed_trace_records();
        return !records.empty();
    } catch (...) {
        records.clear();
        return false;
    }
#else
    (void)convolver;
    return false;
#endif
}

bool drain_gpu_convolver_trial_admissions(
    GpuConvolver& convolver, std::vector<SharedIoTraceAdmission>& admissions) noexcept {
    admissions.clear();
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!convolver.shared_io_ || !convolver.shared_io_->session)
        return false;
    try {
        if (convolver.shared_io_->session->prepared() && !convolver.shared_io_->session->release())
            return false;
        if (convolver.shared_io_->session->last_closed_trace_retention_overflow())
            return false;
        admissions = convolver.shared_io_->session->take_last_closed_trace_admissions();
        return !admissions.empty();
    } catch (...) {
        admissions.clear();
        return false;
    }
#else
    (void)convolver;
    return false;
#endif
}

std::uint64_t gpu_convolver_trial_engine_id(const GpuConvolver& convolver) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!convolver.shared_io_ || !convolver.shared_io_->session)
        return 0;
    return convolver.shared_io_->session->provider_diagnostics().engine_id;
#else
    (void)convolver;
    return 0;
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
    // The staged async path emits pipeline_blocks behind its submit; the
    // transport primes that many fewer blocks, so total latency stays
    // latency_blocks.
    d.worker_pipeline_blocks = staged_readback_->pipeline_blocks;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    std::optional<detail::GpuConvolverTrialConfig> pending_trial_config;
    if (shared_io_ && shared_io_->session && !shared_io_->session->release())
        return false;
    if (shared_io_)
        pending_trial_config = std::move(shared_io_->trial_config);
#endif
    shared_io_.reset();
    staged_trial_.reset();
    staged_sync_records_.clear();
    staged_sequence_ = 0;
    // Same order as the destructor: an abandoned readback completes when the
    // device goes, and it may target the staged state and `time_`.
    gpu_.reset();
    {
        auto fresh = std::make_unique<StagedReadbackState>();
        fresh->breaker = staged_readback_->breaker;
        fresh->transport_hosted = staged_readback_->transport_hosted;
        // The node's one link: a hosting transport keeps it across re-prepare.
        fresh->pipeline_link = std::move(staged_readback_->pipeline_link);
        staged_readback_ = std::move(fresh);
    }
    if (channels_ == 0 || block_ == 0 || sample_rate_ == 0 || ir_.empty() || latency_blocks_ == 0 ||
        latency_blocks_ > kMaxLatencyBlocks)
        return false;

    // The GPU transport and its prepared CPU fallback both require the exact
    // host block size. Keep rejecting non-power-of-two blocks: the GPU FFT is
    // radix-2 and the fallback must never silently normalize to a different
    // callback size. FixedIrConvolver itself is prepared only after this gate.
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
    {
        auto& staged = *staged_readback_;
        staged.cpu_tail_length =
            static_cast<std::uint32_t>((ir_.size() - 1u + block_ - 1u) / block_);
        staged.zeros.assign(block_, 0.0f);
        staged.cpu_out.assign(static_cast<std::size_t>(block_) * channels_, 0.0f);
        staged.zero_ptrs.assign(channels_, staged.zeros.data());
        staged.cpu_out_ptrs.resize(channels_);
        for (uint32_t ch = 0; ch < channels_; ++ch)
            staged.cpu_out_ptrs[ch] = staged.cpu_out.data() + static_cast<std::size_t>(ch) * block_;
    }

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto requested_path =
        static_cast<detail::SharedIoRequest>(trial_requested_path_ & kTrialPathMask);
    const auto trial_slots = decode_trial_slots(trial_requested_path_);
    // Diagnostic trial configuration is deliberately authoritative when it is
    // present. Normal SDK callers use the public host-only policy below.
    if (!trial_configured_) {
        switch (provider_policy_) {
        case ProviderPolicy::Auto:
            // Compiling shared support must not change existing SDK callers.
            requested_path = detail::SharedIoRequest::RequireStaged;
            break;
        case ProviderPolicy::StagedOnly:
        case ProviderPolicy::StagedBlocking:
            requested_path = detail::SharedIoRequest::RequireStaged;
            break;
        case ProviderPolicy::SharedRequired:
            requested_path = detail::SharedIoRequest::RequireSharedHostPointer;
            break;
        }
    }
#endif

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (expected_dawn_revision_available() &&
        requested_path != detail::SharedIoRequest::RequireStaged) {
        try {
            constexpr uint32_t kSharedIoCapacity = 8;
            const uint32_t shared_capacity = std::max(kSharedIoCapacity, latency_blocks_ + 1u);
            if (fft_size_ <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                auto state = std::make_unique<SharedIoState>();
                if (pending_trial_config)
                    state->trial_config = std::move(pending_trial_config);
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
                                  trial_configured_ ? (trial_completion_wait_ns_ & kTrialWaitMask)
                                                    : 0},
                         .session =
                             {.pipeline = {.capacity = shared_capacity,
                                           .channels = channels_,
                                           .block_size = block_,
                                           .fft_size = fft_size_,
                                           .ir_length = static_cast<uint32_t>(ir_.size()),
                                           .lead_blocks = latency_blocks_,
                                           .capture_callback_timing =
                                               trial_capture_callback_timing_},
                              .slots = trial_slots != 0 ? trial_slots : kSharedIoSlots,
                              .sample_rate = sample_rate_,
                              .trace = {.success_stride = trial_success_stride_,
                                        .retention_capacity = trial_configured_
                                                                  ? static_cast<std::uint32_t>(
                                                                        trial_completion_wait_ns_ >>
                                                                        kTrialRetentionShift)
                                                                  : 0,
                                        .capture_admissions = trial_capture_admissions_,
                                        .enabled =
                                            pulp::runtime::kTracingEnabled || trial_enable_trace_},
                              .prediction = state->trial_config
                                                ? state->trial_config->prediction
                                                : detail::SharedIoExecutionPredictor::Config{},
                              .calibration_identity =
                                  state->trial_config ? state->trial_config->calibration_identity
                                                      : detail::SharedIoCalibrationIdentity{},
                              .authenticated_calibration =
                                  state->trial_config
                                      ? state->trial_config->authenticated_calibration
                                      : false,
                              .calibration_max_age_ns =
                                  state->trial_config ? state->trial_config->calibration_max_age_ns
                                                      : 0,
                              .calibration_source_revision =
                                  state->trial_config
                                      ? state->trial_config->calibration_source_revision
                                      : std::string{},
                              .completion_service_wait_ns =
                                  completion_service_wait_ns(block_, sample_rate_)},
                         .normalized_ir_spectrum = state->normalized_ir_spectrum});
                    // A failed preparation may still own physically live
                    // storage. Retain that session even when it cannot run.
                    if (created.session) {
                        state->session = std::move(created.session);
                        shared_io_ = std::move(state);
                        if (shared_io_->trial_config && shared_io_->trial_config->observer &&
                            !shared_io_->session->set_trace_observer(
                                shared_io_->trial_config->observer)) {
                            (void)shared_io_->session->release();
                            shared_io_.reset();
                            return false;
                        }
                    }
                }
            }
        } catch (...) {
            shared_io_.reset();
        }
    }
#endif

    // A required shared provider is a fail-closed contract. Do not silently
    // create the legacy standalone device or report an apparently prepared
    // node when authenticated shared storage was unavailable.
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    const bool shared_provider_ready =
        shared_io_ && shared_io_->session && shared_io_->session->prepared();
    if (!trial_configured_ && provider_policy_ == ProviderPolicy::SharedRequired &&
        !shared_provider_ready) {
        gpu_.reset();
        return false;
    }
#else
    if (!trial_configured_ && provider_policy_ == ProviderPolicy::SharedRequired)
        return false;
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
    if (trial_configured_ && requested_path == detail::SharedIoRequest::RequireStaged && gpu_ &&
        !trial_staged_sync_reference_)
        staged_trial_ = std::make_unique<detail::StagedAsyncTrialState>(2, trial_generation_);
#endif

    configure_staged_pipeline();

    prepared_ = true;
    return true;
}

std::uint8_t GpuConvolver::process_realtime_shared_io(void* self,
                                                      const audio::BufferView<const float>& input,
                                                      audio::BufferView<float>& output,
                                                      std::uint32_t n, std::uint64_t sequence,
                                                      bool input_valid,
                                                      std::uint64_t callback_start_ns) noexcept {
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
        std::span<const float>(state.callback_input.data(), state.callback_input.size()), sequence,
        callback_start_ns);
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
    // quiescent accessor drains it. The ordinary runtime path, or an installed
    // complete observer, drains here; never race a second diagnostic consumer
    // against this SPSC queue.
    if ((!convolver->trial_configured_ && !convolver->trial_enable_trace_) ||
        convolver->shared_io_->session->trace_observer_enabled())
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
                                               std::uint8_t disposition,
                                               std::uint64_t callback_end_ns,
                                               std::uint64_t result_visible_ns) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (!convolver || !convolver->shared_io_ || !convolver->shared_io_->session)
        return;
    auto& state = *convolver->shared_io_;
    if (state.callback.valid() && state.callback.stamp.sequence == sequence)
        (void)state.session->complete_callback_delivery(
            state.callback, static_cast<detail::SharedIoDeliveryDisposition>(disposition),
            callback_end_ns, result_visible_ns);
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

detail::SharedIoProviderIdentity GpuConvolver::provider_identity_for_diagnostics() const noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (shared_io_ != nullptr && shared_io_->session != nullptr)
        return shared_io_->session->provider_identity();
#endif
    return {};
}

bool GpuConvolver::bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept {
    binding.pipeline = staged_readback_->pipeline_link;
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    binding.identity_context = this;
    binding.identity = [](const void* self) noexcept {
        return static_cast<const GpuConvolver*>(self)->provider_identity_for_diagnostics();
    };
    if (has_realtime_shared_io())
        binding.path = {.context = this,
                        .process = &GpuConvolver::process_realtime_shared_io,
                        .service = &GpuConvolver::service_realtime_shared_io,
                        .fence = &GpuConvolver::fence_realtime_shared_io,
                        .delivered = &GpuConvolver::complete_realtime_shared_io,
                        .next_sequence = &GpuConvolver::next_realtime_shared_io_sequence,
                        .health = &GpuConvolver::realtime_shared_io_health,
                        .set_completion_notifier = &GpuConvolver::set_realtime_shared_io_notifier};
#endif
    return true;
}

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
namespace {
GpuAudioRecoveryReason public_recovery_reason(detail::SharedIoRecoveryReason reason) noexcept {
    switch (reason) {
    case detail::SharedIoRecoveryReason::None:
        return GpuAudioRecoveryReason::None;
    case detail::SharedIoRecoveryReason::SequenceGap:
        return GpuAudioRecoveryReason::SequenceGap;
    case detail::SharedIoRecoveryReason::InputSaturated:
        return GpuAudioRecoveryReason::InputSaturated;
    case detail::SharedIoRecoveryReason::ProviderFailure:
        return GpuAudioRecoveryReason::ProviderFailure;
    case detail::SharedIoRecoveryReason::ProviderLost:
        return GpuAudioRecoveryReason::ProviderLost;
    case detail::SharedIoRecoveryReason::OfflineFence:
        return GpuAudioRecoveryReason::OfflineFence;
    case detail::SharedIoRecoveryReason::InvalidCallback:
        return GpuAudioRecoveryReason::InvalidCallback;
    case detail::SharedIoRecoveryReason::ProviderUnresponsive:
        return GpuAudioRecoveryReason::ProviderUnresponsive;
    }
    return GpuAudioRecoveryReason::None;
}
} // namespace
#endif

bool GpuConvolver::set_realtime_shared_io_notifier(
    void* self, detail::SharedIoCompletionNotifier notifier) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = static_cast<GpuConvolver*>(self);
    return convolver != nullptr && convolver->shared_io_ && convolver->shared_io_->session &&
           convolver->shared_io_->session->set_completion_notifier(notifier);
#else
    (void)self;
    (void)notifier;
    return false;
#endif
}

bool GpuConvolver::realtime_shared_io_health(const void* self,
                                             detail::RealtimeGpuHealth& out) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    const auto* convolver = static_cast<const GpuConvolver*>(self);
    if (convolver == nullptr || !convolver->shared_io_ || !convolver->shared_io_->session)
        return false;
    const auto& session = *convolver->shared_io_->session;
    detail::RealtimeGpuHealth health;
    health.recovery_reason = public_recovery_reason(session.recovery_reason());
    health.recovery_sequence = session.recovery_sequence();
    // A cause the worker latched without a stream position (provider loss,
    // failure or an offline fence) is attributed to the first sequence of the
    // epoch it fenced; the exact position is unknown.
    if (health.recovery_reason != GpuAudioRecoveryReason::None &&
        health.recovery_sequence == std::numeric_limits<std::uint64_t>::max())
        health.recovery_sequence = session.epoch_first_sequence();
    health.epoch = session.epoch();
    health.delivery_epoch_active = session.delivery_epoch() != 0;
    health.device_lost = session.provider_device_lost();
    health.fenced = !session.prepared() || session.fenced() || !health.delivery_epoch_active;
    health.gpu_kernel_available = true;
    // Same mapping as the WaveNet node: a failed GPU block is covered by this
    // node's declared CPU fallback.
    using Gpu = detail::SharedIoGpuTerminalDisposition;
    const auto tally = [&](GpuAudioTerminalDisposition disposition, Gpu terminal) noexcept {
        health.terminal_counts[static_cast<std::size_t>(disposition)] +=
            session.gpu_terminal_count(terminal);
    };
    tally(GpuAudioTerminalDisposition::GpuDelivered, Gpu::CompletedAccepted);
    tally(GpuAudioTerminalDisposition::CpuFallback, Gpu::ProviderFailed);
    tally(GpuAudioTerminalDisposition::StaleRejected, Gpu::StaleRejected);
    tally(GpuAudioTerminalDisposition::LateRejected, Gpu::LateRejected);
    tally(GpuAudioTerminalDisposition::DeviceLost, Gpu::DeviceLost);
    tally(GpuAudioTerminalDisposition::Cancelled, Gpu::CancelledTeardown);
    (void)convolver->shared_io_->health.publish(health);
    out = health;
    return true;
#else
    (void)self;
    (void)out;
    return false;
#endif
}

GpuAudioRecoveryReason GpuConvolver::staged_recovery_reason() const noexcept {
    // The worker's latch, or a loss it has not processed a block since.
    const auto latched = staged_latched_reason();
    if (latched == GpuAudioRecoveryReason::ProviderLost)
        return latched;
    if (gpu_ != nullptr && gpu_->device_lost())
        return GpuAudioRecoveryReason::ProviderLost;
    return latched;
}

GpuAudioStatus GpuConvolver::status_snapshot() const noexcept {
    GpuAudioStatus status;
    if (!prepared_) {
        status.provider_state = GpuAudioProviderState::Uninitialized;
        return status;
    }
    status.requested_engine =
        provider_policy_ == ProviderPolicy::Auto ? GpuAudioEngine::Unknown : GpuAudioEngine::Gpu;
    status.fallback_policy = MissPolicy::CpuFallback;
    status.sample_rate = sample_rate_;
    status.block_size = block_;
    status.latency_samples = latency_blocks_ * block_;
    status.fallback_available = cpu_fallback_prepared();
    status.diagnostics_available = true;

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (has_realtime_shared_io()) {
        const auto telemetry = shared_io_->session->trace_telemetry();
        // The shared provider names itself; an unauthenticated one is not a
        // GPU claim.
        const auto identity = provider_identity_for_diagnostics();
        status.provider = identity.authenticated ? identity.provider : GpuAudioProvider::Unknown;
        status.gpu_kernel_available = true;
        status.selected_engine = status.provider != GpuAudioProvider::Unknown
                                     ? GpuAudioEngine::Gpu
                                     : GpuAudioEngine::Unknown;
        status.prepared_lead_blocks = latency_blocks_;
        status.produced_blocks = telemetry.delivered_blocks;
        status.missed_blocks = telemetry.deadline_misses;
        status.fallback_blocks = telemetry.fallback_blocks;
        status.resynced_blocks = telemetry.resync_drops;
        status.input_dropped_frames = telemetry.input_drops;
        detail::RealtimeGpuHealth health;
        if (shared_io_->health.read(health)) {
            status.provider_state = detail::provider_state_for_health(health);
            status.recovery_reason = detail::reported_recovery_reason(health);
            status.recovery_sequence = health.recovery_sequence;
            status.epoch = health.epoch;
            status.delivery_epoch_active = health.delivery_epoch_active;
            // The node's delivery counters above come from trace telemetry,
            // which only records while tracing; without them the terminal
            // counts could run ahead of the deliveries they describe. The
            // hosting transport reports both from its own counters.
            if (shared_io_->session->trace_recording_enabled()) {
                for (std::size_t i = 0; i < GpuAudioStatus::kTerminalDispositionCount; ++i)
                    status.terminal_counts[i] = health.terminal_counts[i];
            }
        } else {
            // No serialized sample yet (or none stable): the provider is
            // authenticated but its delivery state is unverified.
            status.provider_state = GpuAudioProviderState::Degraded;
        }
        // Fenced or lost: the continuously primed CPU fallback is delivering.
        status.selected_engine = detail::delivering_engine(
            status.provider_state, status.selected_engine, status.fallback_policy);
        return status;
    }
#endif

    // Staged path. A live GpuCompute device does not identify the provider or
    // prove which engine produced a given block, so both stay Unknown; with
    // no device the worker runs the exact CPU convolution.
    status.provider = GpuAudioProvider::Unknown;
    status.selected_engine = gpu_ != nullptr ? GpuAudioEngine::Unknown : GpuAudioEngine::Cpu;
    status.gpu_kernel_available = gpu_ != nullptr;
    status.recovery_reason = staged_recovery_reason();
    if (status.recovery_reason != GpuAudioRecoveryReason::None) {
        // The worker block at which the cause was latched; a loss the worker
        // has not reached yet is attributed to the next block it will run.
        status.recovery_sequence =
            staged_latched_reason() == status.recovery_reason
                ? staged_readback_->reason_sequence.load(std::memory_order_acquire)
                : staged_readback_->calls.load(std::memory_order_relaxed);
    }
    // A tripped breaker or a lost device leaves the worker's CPU convolution
    // delivering every block.
    if (staged_readback_->cpu_mode.load(std::memory_order_acquire) ||
        status.recovery_reason == GpuAudioRecoveryReason::ProviderLost)
        status.selected_engine = GpuAudioEngine::Cpu;
    status.provider_state = detail::provider_loss_reason(status.recovery_reason)
                                ? GpuAudioProviderState::Unavailable
                                : GpuAudioProviderState::Degraded;
    return status;
}

render::GpuCompute*
detail::GpuConvolverTestAccess::staged_compute(GpuConvolver& convolver) noexcept {
    return convolver.gpu_.get();
}

void detail::GpuConvolverTestAccess::install_staged_compute(
    GpuConvolver& convolver, std::unique_ptr<render::GpuCompute> compute) noexcept {
    convolver.gpu_ = std::move(compute);
    // The staged pipeline depth follows the device; re-derive it.
    convolver.configure_staged_pipeline();
}

bool detail::GpuConvolverTestAccess::install_shared_io_session(
    GpuConvolver& convolver, std::unique_ptr<SharedIoConvolutionSession>&& session) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!convolver.prepared_ || !session || !session->prepared())
        return false;
    try {
        auto state = std::make_unique<GpuConvolver::SharedIoState>();
        const auto samples = static_cast<std::size_t>(convolver.channels_) * convolver.block_;
        state->callback_input.assign(samples, 0.0f);
        state->callback_output.assign(samples, 0.0f);
        state->session = std::move(session);
        convolver.gpu_.reset();
        convolver.shared_io_ = std::move(state);
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)convolver;
    (void)session;
    return false;
#endif
}

GpuAudioRecoveryReason GpuConvolver::recovery_reason() const noexcept {
    return status_snapshot().recovery_reason;
}

bool GpuConvolver::fenced() const noexcept {
    const auto status = status_snapshot();
    if (status.provider_state == GpuAudioProviderState::Uninitialized)
        return true;
    // Only a sampled, unfenced shared session is Ready; anything else keeps
    // callbacks on the CPU fallback.
    if (has_realtime_shared_io())
        return status.provider_state != GpuAudioProviderState::Ready;
    return status.recovery_reason != GpuAudioRecoveryReason::None;
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
        const auto async_before = gpu_->async_stats();
        // GpuCompute guarantees that poll_readbacks() resolves every request at
        // success or at its deadline, so this worker loop is bounded by the
        // provider deadline rather than an unbounded wait on GPU progress.
        while (!completion->done)
            (void)gpu_->poll_readbacks();
        const auto async_after = gpu_->async_stats();
        detail::SharedIoTransferCounters transfer_counters;
        transfer_counters.write_buffer_calls =
            async_after.write_buffer_calls - async_before.write_buffer_calls;
        transfer_counters.write_buffer_bytes =
            async_after.write_buffer_bytes - async_before.write_buffer_bytes;
        transfer_counters.output_copy_calls =
            async_after.output_copy_calls - async_before.output_copy_calls;
        transfer_counters.output_copy_bytes =
            async_after.output_copy_bytes - async_before.output_copy_bytes;
        transfer_counters.map_async_calls =
            async_after.map_async_calls - async_before.map_async_calls;
        transfer_counters.mapped_readback_memcpy_calls =
            async_after.mapped_readback_memcpy_calls - async_before.mapped_readback_memcpy_calls;
        transfer_counters.mapped_readback_memcpy_bytes =
            async_after.mapped_readback_memcpy_bytes - async_before.mapped_readback_memcpy_bytes;
        (void)staged_trial_->set_transfer_counters(completion->id, transfer_counters);
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

    // Explicit P4 reference path. This is the pre-existing blocking staged
    // GpuCompute operation, instrumented at its actual call boundaries. It is
    // selected only by the host-only staged_sync_reference trial flag and is
    // never reachable from the realtime shared-I/O callback.
    if (trial_staged_sync_reference_) {
        const auto now_ns = []() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        };
        detail::SharedIoTraceRecord trace;
        trace.generation = trial_generation_;
        trace.sequence = staged_sequence_++;
        trace.gpu_work_admitted = true;
        trace.set(detail::SharedIoTraceStage::Scheduled, now_ns());
        trace.set(detail::SharedIoTraceStage::WorkerEntry, now_ns());
        const auto before = gpu_->async_stats();
        trace.set(detail::SharedIoTraceStage::EncodeBegin, now_ns());
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        const uint32_t direct_cplx = fft_size_ * 2u;
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            const float* x = input.channel_ptr(ch);
            float* slot = in_pad_.data() + static_cast<std::size_t>(ch) * direct_cplx;
            for (uint32_t i = 0; i < n; ++i)
                slot[2u * i] = x[i];
        }
        trace.set(detail::SharedIoTraceStage::EncodeEnd, now_ns());
        trace.set(detail::SharedIoTraceStage::SubmitBegin, now_ns());
        const bool success =
            gpu_->convolve_batch(in_pad_.data(), time_.data(), fft_size_, channels_);
        trace.set(detail::SharedIoTraceStage::SubmitEnd, now_ns());
        trace.set(detail::SharedIoTraceStage::CompletionObserved, now_ns());
        const auto after = gpu_->async_stats();
        trace.set(detail::SharedIoTraceStage::RetirementObserved, now_ns());
        trace.transfer_counters = {
            after.write_buffer_calls - before.write_buffer_calls,
            after.write_buffer_bytes - before.write_buffer_bytes,
            after.output_copy_calls - before.output_copy_calls,
            after.output_copy_bytes - before.output_copy_bytes,
            after.map_async_calls - before.map_async_calls,
            after.mapped_readback_memcpy_calls - before.mapped_readback_memcpy_calls,
            after.mapped_readback_memcpy_bytes - before.mapped_readback_memcpy_bytes};
        trace.transfer_counters_available = true;
        trace.cpu_detail_available = true;
        trace.outcome = success ? detail::SharedIoTraceOutcome::Success
                                : detail::SharedIoTraceOutcome::CompletionFailed;
        trace.gpu_terminal = success ? detail::SharedIoGpuTerminalDisposition::CompletedAccepted
                                     : detail::SharedIoGpuTerminalDisposition::ProviderFailed;
        if (!success) {
            trace.reason = detail::SharedIoFallbackReason::CompletionFailed;
            trace.gpu_reason = detail::SharedIoFallbackReason::CompletionFailed;
            output.clear();
        } else {
            for (uint32_t ch = 0; ch < channels_; ++ch)
                detail::overlap_add_block(carry_[ch].data(),
                                          time_.data() + static_cast<std::size_t>(ch) * direct_cplx,
                                          /*src_stride=*/2, output.channel_ptr(ch), fft_size_, n);
        }
        // The record is retained only after all fields are complete. Invalid
        // records are rejected rather than turned into synthetic evidence.
        if (trace.valid())
            staged_sync_records_.push_back(trace);
        return;
    }

    auto& staged = *staged_readback_;
    const std::uint32_t pipeline = staged.pipeline_blocks;
    const std::uint64_t call = staged.calls.fetch_add(1, std::memory_order_relaxed);
    auto& submit_slot = staged.slots[call % staged.slot_count];

    const bool lost = gpu_->device_lost();
    if (lost)
        latch_staged_recovery(GpuAudioRecoveryReason::ProviderLost);

    // Keep this block's input: it is convolved on the CPU if its GPU result is
    // not in when the block is due.
    for (uint32_t ch = 0; ch < channels_; ++ch)
        std::copy_n(input.channel_ptr(ch), n,
                    submit_slot.input.data() + static_cast<std::size_t>(ch) * block_);

    // Decide whether this block goes to the GPU. A lost device never does; a
    // tripped breaker only for its periodic probe.
    bool attempt = !lost;
    if (attempt && staged.tripped) {
        if (staged.probe_countdown > 0)
            --staged.probe_countdown;
        attempt = staged.probe_countdown == 0;
        // One probe per interval: the next waits for this one's verdict.
        if (attempt)
            staged.probe_countdown = staged.breaker.probe_interval_blocks;
    }
    submit_slot.id = 0;
    submit_slot.attempted = attempt;
    submit_slot.done = false;
    submit_slot.status = render::GpuCompute::ReadbackStatus::Failed;
    if (attempt) {
        // Pack every channel's zero-padded complex block back to back.
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            const float* x = input.channel_ptr(ch);
            float* slot = in_pad_.data() + static_cast<std::size_t>(ch) * cplx;
            for (uint32_t i = 0; i < n; ++i)
                slot[2u * i] = x[i];
        }
        // ONE fused, GPU-resident convolution for all channels — forward FFT,
        // complex multiply by the resident IR spectrum, inverse FFT — in a
        // single submit with a single readback.
        if (pipeline > 0) {
            submit_staged_readback(staged, call % staged.slot_count);
        } else if (provider_policy_ == ProviderPolicy::StagedBlocking) {
            submit_slot.done = gpu_->convolve_batch(in_pad_.data(), submit_slot.result.data(),
                                                    fft_size_, channels_);
            submit_slot.status = submit_slot.done ? render::GpuCompute::ReadbackStatus::Success
                                                  : render::GpuCompute::ReadbackStatus::Failed;
        } else {
            submit_slot.done = wait_staged_readback(staged, submit_slot.result.data());
            submit_slot.status =
                staged.done ? staged.status : render::GpuCompute::ReadbackStatus::Expired;
            submit_slot.done = staged.done;
        }
    }

    // Collect whatever has landed, without waiting.
    if (pipeline > 0)
        (void)gpu_->poll_readbacks();

    // The block due now was submitted `pipeline` calls ago. Until the pipeline
    // has filled there is nothing due; the transport primed one block less.
    if (call < pipeline) {
        output.clear();
        return;
    }
    auto& due = staged.slots[(call - pipeline) % staged.slot_count];
    // On time is judged by the clock, not by the call count: a block's result
    // is due pipeline_blocks periods after its submit. A live worker reaches
    // this call at or after that point, so it never waits here. A caller that
    // runs ahead of real time (an offline bounce, a worker catching up) polls,
    // bounded by that due time, so it does not misjudge a result that is
    // simply not due yet.
    if (pipeline > 0 && due.attempted && !due.done && due.id != 0) {
        const auto period = std::chrono::nanoseconds(static_cast<std::uint64_t>(block_) *
                                                     1'000'000'000ull / sample_rate_);
        const auto due_at = due.submitted + period * pipeline;
        if (std::chrono::steady_clock::now() < due_at) {
            while (!due.done && std::chrono::steady_clock::now() < due_at)
                (void)gpu_->poll_readbacks();
            // One last poll once the due time has passed, so a thread that
            // was preempted across it still collects a result that landed.
            if (!due.done)
                (void)gpu_->poll_readbacks();
        }
    }
    bool gpu_ok =
        due.attempted && due.done && due.status == render::GpuCompute::ReadbackStatus::Success;
    // A result that has not landed is now late; its completion is discarded.
    due.id = 0;
    if (gpu_ok) {
        // A non-finite readback in ANY channel routes the whole block to the
        // CPU, by design: the block's input then lands in one place for every
        // channel, and no carry is reset (resetting would drop the IR tail of
        // every earlier block).
        const std::size_t reals = static_cast<std::size_t>(channels_) * fft_size_;
        for (std::size_t i = 0; i < reals; ++i) {
            if (!std::isfinite(due.result[2u * i])) {
                gpu_ok = false;
                latch_staged_recovery(GpuAudioRecoveryReason::ProviderFailure);
                break;
            }
        }
    } else if (due.attempted) {
        if (gpu_->device_lost()) {
            latch_staged_recovery(GpuAudioRecoveryReason::ProviderLost);
        } else if (due.done && due.status == render::GpuCompute::ReadbackStatus::Failed) {
            // A failed map is a provider error. A late or rejected result is a
            // served miss that only the breaker counts.
            latch_staged_recovery(GpuAudioRecoveryReason::ProviderFailure);
        }
    }
    if (due.attempted)
        update_staged_breaker(staged, gpu_ok);
    if (gpu_ok)
        ++staged.gpu_blocks;

    // Why the output is exact whichever path a block takes. Convolution is
    // linear and time-invariant: y = h * x = sum over blocks k of h * x_k,
    // where x_k is input block k placed at its time. Every block's x_k is
    // added to exactly one of two accumulators, never both and never neither:
    //   - the GPU carry (overlap-add of h * x_k, fft_size long, shifted one
    //     block per call) when its readback is on time and finite, or
    //   - the worker's CPU convolution (FixedIrConvolver), a zero-latency
    //     implementation of the same h, fed x_k when the GPU did not deliver
    //     it and silence otherwise.
    // Both accumulators advance one block per call on the same timeline, so
    // their sum at any block is sum_k h * x_k = y. The CPU convolution's
    // contribution from its last real input ends ceil((L - 1) / n) blocks
    // later (L = IR length, n = block); after that it holds only rounding
    // residue, which is cleared, so it can sit idle without changing y. No
    // history replay is needed because nothing ever has to be moved from one
    // accumulator to the other.
    //
    // GPU carry: add the due block's result when it arrived, emit the head,
    // shift. A block the GPU did not deliver adds nothing; the carry still
    // advances so it stays on the timeline.
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        float* carry = carry_[ch].data();
        float* out = output.channel_ptr(ch);
        if (gpu_ok) {
            (void)detail::overlap_add_block(carry,
                                            due.result.data() + static_cast<std::size_t>(ch) * cplx,
                                            /*src_stride=*/2, out, fft_size_, n);
        } else {
            std::copy_n(carry, n, out);
            std::copy(carry + n, carry + fft_size_, carry);
            std::fill(carry + (fft_size_ - n), carry + fft_size_, 0.0f);
        }
    }
    for (uint32_t ch = channels_; ch < output.num_channels(); ++ch)
        std::fill_n(output.channel_ptr(ch), n, 0.0f);

    // CPU convolution: takes the due block's input when the GPU did not, and
    // otherwise runs on silence only while it still owes the tail of earlier
    // CPU blocks. Past that tail it is idle and costs nothing.
    if (gpu_ok && staged.cpu_tail_blocks == 0)
        return;
    const auto cpu_t0 = std::chrono::steady_clock::now();
    audio::BufferView<const float> zero_in(staged.zero_ptrs.data(), channels_, n);
    audio::BufferView<const float> due_in(due.input_ptrs.data(), channels_, n);
    audio::BufferView<float> cpu_view(staged.cpu_out_ptrs.data(), channels_, n);
    render_worker_fallback(gpu_ok ? zero_in : due_in, cpu_view, n);
    ++staged.cpu_blocks;
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        float* out = output.channel_ptr(ch);
        const float* cpu = staged.cpu_out_ptrs[ch];
        for (uint32_t i = 0; i < n; ++i)
            out[i] += cpu[i];
    }
    if (!gpu_ok) {
        staged.cpu_tail_blocks = staged.cpu_tail_length;
    } else if (--staged.cpu_tail_blocks == 0) {
        // The tail has been emitted; clear rounding residue so the next miss
        // starts from silence.
        for (auto& fallback : worker_fallback_)
            fallback.reset();
    }
    const auto cpu_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - cpu_t0)
                                       .count());
    staged.cpu_block_ns = staged.cpu_block_ns == 0
                              ? cpu_ns
                              : staged.cpu_block_ns - staged.cpu_block_ns / 8 + cpu_ns / 8;
}

std::uint32_t GpuConvolver::staged_pipeline_depth() const noexcept {
    return staged_readback_->transport_hosted && gpu_ && !trial_configured_ &&
                   !trial_staged_sync_reference_ &&
                   provider_policy_ != ProviderPolicy::StagedBlocking && latency_blocks_ >= 2
               ? latency_blocks_ - 1u
               : 0u;
}

void GpuConvolver::configure_staged_pipeline() {
    // When a transport hosts the node, the staged async path gives the GPU all
    // of the lead but one block: the result for block N is judged
    // latency_blocks - 1 calls later, and the one primed block left is the
    // worker's own scheduling slack (one block at the default lead of 2). The
    // transport primes that many fewer blocks, so total latency is unchanged.
    // Called directly, the node keeps block N in, block N out, and decides each
    // block inside its call (bounded wait). So do the blocking escape hatch,
    // the diagnostic trial paths, a node with no staged device and a one-block
    // lead.
    auto& staged = *staged_readback_;
    staged.pipeline_blocks = staged_pipeline_depth();
    staged.calls.store(0, std::memory_order_relaxed);
    staged.slot_count = staged.pipeline_blocks + 1u;
    // The provider rejects a submit past its in-flight cap (default 4), so
    // the cap must cover every request the pipeline keeps in flight, plus as
    // many abandoned late ones still holding a slot.
    if (gpu_ && staged.pipeline_blocks > 0)
        gpu_->set_max_readbacks_in_flight(
            std::max<std::size_t>(4, 2u * static_cast<std::size_t>(staged.slot_count)));
    if (staged.slots.size() < staged.slot_count)
        staged.slots.resize(staged.slot_count);
    const auto cplx = static_cast<std::size_t>(fft_size_) * 2u;
    for (auto& slot : staged.slots) {
        // A completion still in flight is ignored from here on (its id no
        // longer matches); its buffers stay allocated.
        slot.id = 0;
        slot.attempted = false;
        slot.done = false;
        slot.status = render::GpuCompute::ReadbackStatus::Failed;
        slot.result.assign(cplx * channels_, 0.0f);
        slot.input.assign(static_cast<std::size_t>(block_) * channels_, 0.0f);
        slot.input_ptrs.resize(channels_);
        for (uint32_t ch = 0; ch < channels_; ++ch)
            slot.input_ptrs[ch] = slot.input.data() + static_cast<std::size_t>(ch) * block_;
    }
    staged.pending_id = 0;
}

bool GpuConvolver::set_staged_pipeline_hosting(void* self, bool hosted) noexcept {
    auto* convolver = static_cast<GpuConvolver*>(self);
    if (!convolver->staged_readback_)
        return false;
    auto& staged = *convolver->staged_readback_;
    if (staged.transport_hosted == hosted)
        return true;
    staged.transport_hosted = hosted;
    // Only a change of depth rebuilds the staged state; a node that never
    // pipelines keeps its block counter and slots across hosting changes.
    if (!convolver->prepared_ || convolver->staged_pipeline_depth() == staged.pipeline_blocks)
        return true;
    try {
        convolver->configure_staged_pipeline();
    } catch (...) {
        return false;
    }
    return true;
}

std::uint64_t GpuConvolver::staged_gpu_blocks() const noexcept {
    return staged_readback_->gpu_blocks;
}

std::uint64_t GpuConvolver::staged_breaker_trips() const noexcept {
    return staged_readback_->breaker_trips;
}

std::uint64_t GpuConvolver::staged_cpu_blocks() const noexcept {
    return staged_readback_->cpu_blocks;
}

void GpuConvolver::update_staged_breaker(StagedReadbackState& staged, bool on_time) noexcept {
    const auto& cfg = staged.breaker;
    if (on_time) {
        staged.consecutive_misses = 0;
        if (staged.tripped) {
            // A probe landed: stay on the GPU on probation.
            staged.tripped = false;
            staged.cpu_mode.store(false, std::memory_order_release);
            staged.on_probation = true;
            staged.probation_hits = 1;
        } else if (staged.on_probation) {
            ++staged.probation_hits;
        }
        if (staged.on_probation && staged.probation_hits >= cfg.recover_after_hits)
            staged.on_probation = false;
        return;
    }
    ++staged.consecutive_misses;
    if (staged.tripped)
        return; // a missed probe (or a block in flight at the trip): keep probing
    if (staged.on_probation || staged.consecutive_misses >= cfg.trip_after_misses) {
        // A GPU that is alive but keeps missing is degraded, not lost.
        latch_staged_recovery(GpuAudioRecoveryReason::SustainedDeadlineMisses);
        staged.tripped = true;
        ++staged.breaker_trips;
        staged.cpu_mode.store(true, std::memory_order_release);
        staged.on_probation = false;
        staged.probation_hits = 0;
        staged.probe_countdown = cfg.probe_interval_blocks;
    }
}

void GpuConvolver::submit_staged_readback(StagedReadbackState& staged, std::size_t slot_index) {
    // The result is due `pipeline_blocks` calls from now and the worker judges
    // it then, on its own. The provider's deadline only has to release a late
    // request's in-flight slot, so it sits one period past the due point: an
    // expiry racing the due call's poll would turn an on-time map into a miss.
    const auto period_ns = static_cast<std::uint64_t>(block_) * 1'000'000'000ull / sample_rate_;
    const auto deadline =
        std::chrono::microseconds(period_ns * (staged.pipeline_blocks + 1u) / 1000ull);
    auto& slot = staged.slots[slot_index];
    StagedReadbackState* state = &staged;
    std::uint64_t id = 0;
    try {
        id = gpu_->convolve_batch_async(
            in_pad_.data(), slot.result.data(), fft_size_, channels_, deadline,
            [state, slot_index](const render::GpuCompute::ReadbackResult& result) {
                auto& target = state->slots[slot_index];
                if (result.id == 0 || result.id != target.id)
                    return;
                target.status = result.status;
                target.done = true;
            });
    } catch (...) {
        id = 0;
    }
    // A rejected submit (in-flight cap reached, no plan) never fires its
    // callback; the slot stays not-done and is judged a miss when due.
    slot.id = id;
    slot.submitted = std::chrono::steady_clock::now();
}

bool GpuConvolver::wait_staged_readback(StagedReadbackState& staged, float* dest) {
    // One-block lead: the block must be decided inside this call. Reserve the
    // CPU convolution a miss would cost plus a margin, and give the readback
    // what is left of one period, so even a missed block fits in a period.
    const auto period_ns = static_cast<std::uint64_t>(block_) * 1'000'000'000ull / sample_rate_;
    const auto reserve_ns = staged.cpu_block_ns + kStagedWaitMarginNs;
    const auto deadline_ns =
        period_ns > reserve_ns + kStagedWaitFloorNs ? period_ns - reserve_ns : kStagedWaitFloorNs;
    const auto deadline = std::chrono::microseconds(deadline_ns / 1000ull);
    const auto give_up = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds(deadline_ns + kStagedGiveUpSlackNs);

    staged.done = false;
    staged.status = render::GpuCompute::ReadbackStatus::Failed;
    StagedReadbackState* state = &staged;
    std::uint64_t id = 0;
    try {
        id = gpu_->convolve_batch_async(in_pad_.data(), dest, fft_size_, channels_, deadline,
                                        [state](const render::GpuCompute::ReadbackResult& result) {
                                            if (result.id != state->pending_id)
                                                return;
                                            state->status = result.status;
                                            state->done = true;
                                        });
    } catch (...) {
        id = 0;
    }
    if (id == 0)
        return false;
    staged.pending_id = id;

    while (!staged.done) {
        (void)gpu_->poll_readbacks();
        if (staged.done || std::chrono::steady_clock::now() >= give_up)
            break;
    }
    const bool ok = staged.done && staged.status == render::GpuCompute::ReadbackStatus::Success;
    // An abandoned request may still complete later. Its completion is ignored
    // by id, and GpuCompute writes `dest` only for a request that succeeded, so
    // a late success is overwritten by the next readback before it is read.
    staged.pending_id = 0;
    return ok;
}

void GpuConvolver::latch_staged_recovery(GpuAudioRecoveryReason reason) noexcept {
    // Only the worker latches. First cause wins, except that a device loss
    // replaces any other cause.
    auto& staged = *staged_readback_;
    const auto latched =
        static_cast<GpuAudioRecoveryReason>(staged.reason.load(std::memory_order_acquire));
    const bool replace = latched == GpuAudioRecoveryReason::None ||
                         (reason == GpuAudioRecoveryReason::ProviderLost &&
                          latched != GpuAudioRecoveryReason::ProviderLost);
    if (!replace)
        return;
    const auto calls = staged.calls.load(std::memory_order_relaxed);
    staged.reason_sequence.store(calls > 0 ? calls - 1u : 0u, std::memory_order_relaxed);
    staged.reason.store(static_cast<std::uint8_t>(reason), std::memory_order_release);
}

} // namespace pulp::gpu_audio
