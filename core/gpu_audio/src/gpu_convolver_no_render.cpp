#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>

#include "detail/gpu_convolver_test_access.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/staged_async_trace_ledger.hpp"
#include "detail/worker_pipeline_hosting.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace pulp::gpu_audio {

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response)
    : GpuConvolver(channels, block_size, sample_rate, std::move(impulse_response), kLatencyBlocks) {
}

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response, uint32_t latency_blocks)
    : channels_(channels), block_(block_size), sample_rate_(sample_rate),
      latency_blocks_(latency_blocks), ir_(std::move(impulse_response)),
      staged_readback_(std::make_unique<StagedReadbackState>()) {}

struct GpuConvolver::SharedIoState {};
namespace detail {
bool set_worker_pipeline_hosting(GpuAudioNode*, bool) noexcept {
    return false;
}
} // namespace detail

struct GpuConvolver::StagedReadbackState {
    GpuConvolverStagedBreaker breaker{};
    std::atomic<std::uint8_t> reason{0};
};

std::uint64_t GpuConvolver::staged_cpu_blocks() const noexcept {
    return 0;
}

std::uint64_t GpuConvolver::staged_gpu_blocks() const noexcept {
    return 0;
}

std::uint64_t GpuConvolver::staged_breaker_trips() const noexcept {
    return 0;
}

GpuAudioRecoveryReason GpuConvolver::staged_latched_reason() const noexcept {
    return static_cast<GpuAudioRecoveryReason>(
        staged_readback_->reason.load(std::memory_order_acquire));
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

GpuConvolver::~GpuConvolver() = default;

bool GpuConvolver::gpu_available() const {
    return false;
}

std::string GpuConvolver::backend() const {
    return {};
}

bool GpuConvolver::set_provider_policy(ProviderPolicy policy) noexcept {
    if (prepared_)
        return false;
    provider_policy_ = policy;
    return true;
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

bool GpuConvolver::has_realtime_shared_io() const noexcept {
    return false;
}

GpuAudioNodeDescriptor GpuConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-convolver";
    d.input_channels = channels_;
    d.output_channels = channels_;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    d.latency_blocks = latency_blocks_;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
    if (provider_policy_ == ProviderPolicy::SharedRequired)
        return false;
    if (channels_ == 0 || block_ == 0 || sample_rate_ == 0 || ir_.empty() || latency_blocks_ == 0 ||
        latency_blocks_ > kMaxLatencyBlocks)
        return false;

    // The GPU transport and its prepared CPU fallback both require the exact
    // host block size. Keep rejecting non-power-of-two blocks: the no-render
    // path is the only audio path, so silently normalizing a callback size
    // would turn every block into a fail-closed violation.
    if ((block_ & (block_ - 1u)) != 0u)
        return false;

    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size()))
        fft_size_ <<= 1;

    // Same continuously-fed fallback + latency-alignment delay ring as the render
    // build; here it is the ONLY audio path (no GPU device).
    init_fallback();

    gpu_.reset();
    prepared_ = true;
    return true;
}

void GpuConvolver::process_block(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& output, uint32_t n) {
    if (!prepared_ || n != block_ || input.num_channels() < channels_ ||
        output.num_channels() < channels_) {
        output.clear();
        return;
    }
    render_worker_fallback(input, output, n);
}

// No staged device or shared session exists in this build, so nothing is ever
// latched and no shared health sample is available.
GpuAudioRecoveryReason GpuConvolver::staged_recovery_reason() const noexcept {
    return GpuAudioRecoveryReason::None;
}

bool GpuConvolver::realtime_shared_io_health(const void*, detail::RealtimeGpuHealth&) noexcept {
    return false;
}

render::GpuCompute*
detail::GpuConvolverTestAccess::staged_compute(GpuConvolver& convolver) noexcept {
    return convolver.gpu_.get(); // always null: no staged device in this build
}

void detail::GpuConvolverTestAccess::install_staged_compute(
    GpuConvolver& convolver, std::unique_ptr<render::GpuCompute> compute) noexcept {
    convolver.gpu_ = std::move(compute);
}

bool detail::GpuConvolverTestAccess::install_shared_io_session(
    GpuConvolver&, std::unique_ptr<SharedIoConvolutionSession>&&) noexcept {
    return false; // shared-I/O support is not compiled into this build
}

// No render backend: the worker always runs the exact CPU convolution. This is
// the staged path's no-device case in the render build, so it reports the same
// way: provider unknown, CPU engine, Degraded, no GPU kernel.
GpuAudioStatus GpuConvolver::status_snapshot() const noexcept {
    GpuAudioStatus status;
    if (!prepared_) {
        status.provider_state = GpuAudioProviderState::Uninitialized;
        return status;
    }
    status.requested_engine =
        provider_policy_ == ProviderPolicy::Auto ? GpuAudioEngine::Unknown : GpuAudioEngine::Gpu;
    status.selected_engine = GpuAudioEngine::Cpu;
    status.provider = GpuAudioProvider::Unknown;
    status.provider_state = GpuAudioProviderState::Degraded;
    status.fallback_policy = MissPolicy::CpuFallback;
    status.fallback_available = cpu_fallback_prepared();
    status.diagnostics_available = true;
    status.sample_rate = sample_rate_;
    status.block_size = block_;
    status.latency_samples = latency_blocks_ * block_;
    return status;
}

GpuAudioRecoveryReason GpuConvolver::recovery_reason() const noexcept {
    return status_snapshot().recovery_reason;
}

bool GpuConvolver::fenced() const noexcept {
    return status_snapshot().provider_state == GpuAudioProviderState::Uninitialized;
}

GpuMultiConvolver::GpuMultiConvolver(uint32_t block_size, uint32_t sample_rate,
                                     std::vector<std::vector<float>> irs)
    : block_(block_size), sample_rate_(sample_rate), irs_(std::move(irs)),
      num_ir_(static_cast<uint32_t>(irs_.size())) {}

GpuMultiConvolver::~GpuMultiConvolver() = default;

GpuAudioNodeDescriptor GpuMultiConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-multi-convolver";
    d.input_channels = 2;
    d.output_channels = 2;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    d.latency_blocks = 2;
    d.miss_policy = MissPolicy::Silence;
    d.supports_cpu_fallback = false;
    return d;
}

std::string GpuMultiConvolver::backend() const {
    return {};
}

bool GpuMultiConvolver::device_lost() const noexcept {
    return false;
}

bool GpuMultiConvolver::prepare_on(std::unique_ptr<render::GpuCompute>) {
    prepared_ = false;
    gpu_.reset();
    return false;
}

namespace detail {
bool prepare_gpu_multi_convolver_with(GpuMultiConvolver&,
                                      std::unique_ptr<render::GpuCompute>) noexcept {
    return false;
}
} // namespace detail

bool GpuMultiConvolver::prepare() {
    prepared_ = false;
    gpu_.reset();
    return false;
}

bool GpuMultiConvolver::convolve_stereo(const float*, float*, float*, uint32_t) {
    return false;
}

void GpuMultiConvolver::process_block(const audio::BufferView<const float>&,
                                      audio::BufferView<float>& output, uint32_t) {
    output.clear();
}

void GpuMultiConvolver::process_cpu_fallback(const audio::BufferView<const float>&,
                                             audio::BufferView<float>& output, uint32_t) noexcept {
    output.clear();
}

// No render backend: prepare() always refuses, so the node never leaves the
// uninitialized state and never delivers GPU audio.
GpuAudioStatus GpuMultiConvolver::status_snapshot() const noexcept {
    GpuAudioStatus status;
    status.provider_state = GpuAudioProviderState::Uninitialized;
    return status;
}

GpuAudioRecoveryReason GpuMultiConvolver::recovery_reason() const noexcept {
    return status_snapshot().recovery_reason;
}

bool GpuMultiConvolver::fenced() const noexcept {
    return true;
}

} // namespace pulp::gpu_audio
