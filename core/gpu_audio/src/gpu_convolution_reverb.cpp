#include <pulp/gpu_audio/gpu_convolution_reverb.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulp::gpu_audio {

namespace {
std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1u)
        return value;
    std::uint32_t result = 1u;
    while (result < value && result <= (std::numeric_limits<std::uint32_t>::max() >> 1u))
        result <<= 1u;
    return result < value ? 0u : result;
}
} // namespace

struct GpuConvolutionReverb::Lane {
    std::vector<float> ir;
    std::vector<float> input;
    std::vector<float> output;
    std::unique_ptr<GpuConvolver> node;
    std::unique_ptr<GpuAudioTransport> transport;
};

GpuConvolutionReverb::GpuConvolutionReverb(GpuConvolutionReverbConfig config)
    : config_(std::move(config)), lanes_{} {
    // The Dawn/FFT transport requires a radix-2 quantum, while a host's
    // prepared max block is only a capacity and is often not a power of two
    // (for example 192).  Round up once, off the audio thread; callbacks up to
    // the original capacity are accumulated into this internal quantum.
    if (config_.block_size != 0u &&
        (config_.block_size & (config_.block_size - 1u)) != 0u) {
        const auto rounded = next_power_of_two(config_.block_size);
        if (rounded != 0u)
            config_.block_size = rounded;
    }
}

GpuConvolutionReverb::~GpuConvolutionReverb() { release(); }

bool GpuConvolutionReverb::valid_config() const noexcept {
    if (!config_.gpu_enabled || config_.block_size == 0 || config_.sample_rate == 0 ||
        !std::isfinite(config_.impulse_response_sample_rate) ||
        config_.impulse_response_sample_rate <= 0.0 ||
        config_.block_size > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config_.block_size > std::numeric_limits<std::uint32_t>::max() / 3u ||
        (config_.block_size & (config_.block_size - 1u)) != 0u || config_.ring_blocks < 4u)
        return false;
    if (config_.impulse_response.size() != 1u && config_.impulse_response.size() != 2u)
        return false;
    const auto length = config_.impulse_response.front().size();
    if (length == 0 || length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    for (const auto& channel : config_.impulse_response) {
        if (channel.size() != length)
            return false;
        for (const float sample : channel)
            if (!std::isfinite(sample))
                return false;
    }
    return true;
}

bool GpuConvolutionReverb::prepare_lanes() noexcept {
    try {
        signal::ZeroLatencyConvolver prepared;
        prepared.prepare(static_cast<double>(config_.sample_rate),
                         static_cast<int>(config_.block_size), 2);
        prepared.set_normalize_mode(config_.normalize);
        prepared.set_tail_trim_db(config_.tail_trim_db);
        prepared.set_tail_fade_ms(config_.tail_fade_ms);
        prepared.set_resample_taps_per_phase(config_.resample_taps_per_phase);

        std::vector<const float*> source_ptrs(config_.impulse_response.size());
        for (std::size_t c = 0; c < source_ptrs.size(); ++c)
            source_ptrs[c] = config_.impulse_response[c].data();
        if (!prepared.load_impulse_response(
                source_ptrs.data(), static_cast<int>(source_ptrs.size()),
                static_cast<int>(config_.impulse_response.front().size()),
                config_.impulse_response_sample_rate))
            return false;

        const auto prepared_length = prepared.prepared_ir_length();
        if (prepared_length <= 0 || prepared.prepared_ir_channels() !=
                                         static_cast<int>(config_.impulse_response.size()))
            return false;

        for (std::size_t lane_index = 0; lane_index < lanes_.size(); ++lane_index) {
            auto lane = std::make_unique<Lane>();
            const int ir_channel = config_.impulse_response.size() == 1u
                                       ? 0
                                       : static_cast<int>(lane_index);
            const float* taps = prepared.prepared_ir(ir_channel);
            lane->ir.assign(taps, taps + prepared_length);
            lane->input.assign(config_.block_size, 0.0f);
            lane->output.assign(config_.block_size, 0.0f);

            lane->node = std::make_unique<GpuConvolver>(
                1u, config_.block_size, config_.sample_rate, lane->ir,
                GpuConvolver::kLatencyBlocks);
            if (!lane->node->set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired) ||
                !lane->node->prepare())
                return false;

            lane->transport = std::make_unique<GpuAudioTransport>();
            const GpuAudioTransport::Config transport_config{
                .ring_blocks = config_.ring_blocks,
                .run_worker_thread = true,
                .wake_on_write = true,
            };
            if (!lane->transport->prepare(lane->node.get(), transport_config))
                return false;
            const auto capability = lane->transport->capability_report();
            if (!capability.prepared || capability.provider != GpuAudioProvider::Dawn ||
                capability.path != GpuAudioExecutionPath::SharedMemory ||
                capability.prepared_lead_blocks != GpuConvolver::kLatencyBlocks)
                return false;
            lanes_[lane_index] = std::move(lane);
        }
        return lanes_[0] != nullptr && lanes_[1] != nullptr;
    } catch (...) {
        return false;
    }
}

void GpuConvolutionReverb::update_filter_coefficients() noexcept {
    hp_active_ = lowcut_hz_ > signal::ZeroLatencyConvolver::kLowcutHzMin;
    lp_active_ = highcut_hz_ < signal::ZeroLatencyConvolver::kHighcutHzMax;
    constexpr double two_pi = 6.283185307179586476925286766559;
    const double hp_w = two_pi * lowcut_hz_ / static_cast<double>(config_.sample_rate);
    hp_coef_ = 1.0 / (1.0 + hp_w);
    const double lp_w = two_pi * highcut_hz_ / static_cast<double>(config_.sample_rate);
    lp_coef_ = std::clamp(lp_w / (1.0 + lp_w), 0.0, 1.0);
}

void GpuConvolutionReverb::clear_runtime_buffers() noexcept {
    for (auto& buffer : predelay_ring_)
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    for (auto& buffer : output_fifo_)
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    std::fill(dry_delay_.begin(), dry_delay_.end(), 0.0f);
    std::fill(control_delay_.begin(), control_delay_.end(), ControlSample{});
    for (auto& buffer : dry_due_)
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    std::fill(control_due_.begin(), control_due_.end(), ControlSample{});
    for (auto& lane : lanes_) {
        if (!lane) continue;
        std::fill(lane->input.begin(), lane->input.end(), 0.0f);
        std::fill(lane->output.begin(), lane->output.end(), 0.0f);
    }
    hp_prev_x_ = {};
    hp_z_ = {};
    lp_z_ = {};
    predelay_write_ = 0;
    dry_write_ = 0;
    quantum_fill_ = 0;
    output_fifo_read_ = output_fifo_write_ = output_fifo_available_ = 0;
}

bool GpuConvolutionReverb::prepare() noexcept {
    release();
    if (!valid_config())
        return false;
    if (!prepare_lanes()) {
        release();
        return false;
    }

    try {
        const double max_predelay_samples =
            std::ceil(signal::ZeroLatencyConvolver::kPredelayMsMax *
                      static_cast<double>(config_.sample_rate) / 1000.0);
        if (max_predelay_samples >= static_cast<double>(std::numeric_limits<std::uint32_t>::max() -
                                                          config_.block_size - 1u)) {
            release();
            return false;
        }
        predelay_capacity_ = static_cast<std::uint32_t>(max_predelay_samples) +
                             config_.block_size + 1u;
        predelay_ring_[0].assign(predelay_capacity_, 0.0f);
        predelay_ring_[1].assign(predelay_capacity_, 0.0f);
        dry_due_[0].assign(config_.block_size, 0.0f);
        dry_due_[1].assign(config_.block_size, 0.0f);
        dry_delay_.assign(static_cast<std::size_t>(config_.block_size) * 4u, 0.0f);
        control_delay_.assign(static_cast<std::size_t>(2u * config_.block_size),
                              ControlSample{});
        control_due_.assign(config_.block_size, ControlSample{});
        // At most one full quantum is produced per host callback (callbacks
        // are bounded by the prepared max block).  Two quanta leave room for
        // the newly completed quantum while the current callback drains its
        // due samples, including a short callback that crosses a quantum
        // boundary.
        output_fifo_capacity_ = 2u * config_.block_size;
        output_fifo_[0].assign(output_fifo_capacity_, 0.0f);
        output_fifo_[1].assign(output_fifo_capacity_, 0.0f);
        prepared_ = true;
        update_filter_coefficients();
        set_predelay_ms(predelay_ms_);
        clear_runtime_buffers();
        return true;
    } catch (...) {
        release();
        return false;
    }
}

void GpuConvolutionReverb::release() noexcept {
    prepared_ = false;
    for (auto& lane : lanes_) {
        if (lane && lane->transport)
            lane->transport->release();
        lane.reset();
    }
    dry_delay_.clear();
    control_delay_.clear();
    for (auto& buffer : predelay_ring_)
        buffer.clear();
    for (auto& buffer : dry_due_)
        buffer.clear();
    control_due_.clear();
    for (auto& buffer : output_fifo_)
        buffer.clear();
    predelay_capacity_ = predelay_samples_ = predelay_write_ = dry_write_ = 0;
    quantum_fill_ = 0;
    output_fifo_capacity_ = output_fifo_read_ = output_fifo_write_ = output_fifo_available_ = 0;
    hp_prev_x_ = {};
    hp_z_ = {};
    lp_z_ = {};
}

void GpuConvolutionReverb::set_ir_gain_db(double db) noexcept {
    if (!std::isfinite(db))
        return;
    ir_gain_db_ = std::clamp(db, signal::ZeroLatencyConvolver::kIrGainDbMin,
                             signal::ZeroLatencyConvolver::kIrGainDbMax);
    ir_gain_linear_ = std::pow(10.0, ir_gain_db_ / 20.0);
}

void GpuConvolutionReverb::set_predelay_ms(double ms) noexcept {
    if (!std::isfinite(ms))
        return;
    predelay_ms_ = std::clamp(ms, signal::ZeroLatencyConvolver::kPredelayMsMin,
                              signal::ZeroLatencyConvolver::kPredelayMsMax);
    if (config_.sample_rate == 0 || predelay_capacity_ <= config_.block_size + 1u)
        return;
    const auto samples = static_cast<std::uint64_t>(std::lround(
        predelay_ms_ * static_cast<double>(config_.sample_rate) / 1000.0));
    predelay_samples_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        samples, static_cast<std::uint64_t>(predelay_capacity_ - config_.block_size - 1u)));
}

void GpuConvolutionReverb::set_wet_percent(double percent) noexcept {
    if (std::isfinite(percent))
        wet_gain_ = std::clamp(percent, 0.0, 100.0) / 100.0;
}

void GpuConvolutionReverb::set_dry_percent(double percent) noexcept {
    if (std::isfinite(percent))
        dry_gain_ = std::clamp(percent, 0.0, 100.0) / 100.0;
}

void GpuConvolutionReverb::set_width_percent(double percent) noexcept {
    if (std::isfinite(percent))
        width_ = std::clamp(percent, signal::ZeroLatencyConvolver::kWidthPercentMin,
                            signal::ZeroLatencyConvolver::kWidthPercentMax) /
                 100.0;
}

void GpuConvolutionReverb::set_lowcut_hz(double hz) noexcept {
    if (!std::isfinite(hz))
        return;
    lowcut_hz_ = std::clamp(hz, signal::ZeroLatencyConvolver::kLowcutHzMin,
                            signal::ZeroLatencyConvolver::kLowcutHzMax);
    if (prepared_)
        update_filter_coefficients();
}

void GpuConvolutionReverb::set_highcut_hz(double hz) noexcept {
    if (!std::isfinite(hz))
        return;
    highcut_hz_ = std::clamp(hz, signal::ZeroLatencyConvolver::kHighcutHzMin,
                             signal::ZeroLatencyConvolver::kHighcutHzMax);
    if (prepared_)
        update_filter_coefficients();
}

void GpuConvolutionReverb::process_quantum() noexcept {
    if (quantum_fill_ != config_.block_size || output_fifo_capacity_ < config_.block_size)
        return;

    // The authenticated transports are deliberately called only with their
    // configured full quantum.  Their fixed two-block lead is then represented
    // by the output samples they return, while the host-facing FIFO below
    // preserves that timeline across arbitrary callback boundaries.
    for (auto& lane : lanes_) {
        std::array<const float*, 1> input_ptrs{lane->input.data()};
        std::array<float*, 1> output_ptrs{lane->output.data()};
        const audio::BufferView<const float> lane_input(input_ptrs.data(), 1u,
                                                         config_.block_size);
        audio::BufferView<float> lane_output(output_ptrs.data(), 1u, config_.block_size);
        lane->transport->process(lane_input, lane_output, config_.block_size);
    }

    for (std::uint32_t i = 0; i < config_.block_size; ++i) {
        const auto controls = control_due_[i];
        double wet_l = 0.0;
        double wet_r = 0.0;
        const std::uint32_t write = predelay_write_;
        const std::uint32_t read =
            (write + predelay_capacity_ - controls.predelay) % predelay_capacity_;
        for (std::size_t lane = 0; lane < 2u; ++lane) {
            auto& ring = predelay_ring_[lane];
            ring[write] = lanes_[lane]->output[i];
            const double delayed = ring[read];
            if (lane == 0u)
                wet_l = delayed;
            else
                wet_r = delayed;
        }
        predelay_write_ = (predelay_write_ + 1u) % predelay_capacity_;

        // Match ZeroLatencyConvolver::mix_chunk(): pre-delay and width are
        // both applied to the wet return, after convolution.  Applying the
        // delay to the send changes automated predelay behavior at partition
        // boundaries even though the two paths are LTI in the static case.
        const double mid = (wet_l + wet_r) * 0.5;
        const double side = (wet_l - wet_r) * 0.5 * controls.width;
        wet_l = mid + side;
        wet_r = mid - side;

        const std::uint32_t fifo_index =
            (output_fifo_write_ + i) % output_fifo_capacity_;
        output_fifo_[0][fifo_index] =
            static_cast<float>(dry_due_[0][i] * controls.dry +
                               wet_l * controls.wet * controls.ir_gain);
        output_fifo_[1][fifo_index] =
            static_cast<float>(dry_due_[1][i] * controls.dry +
                               wet_r * controls.wet * controls.ir_gain);
    }
    output_fifo_write_ = (output_fifo_write_ + config_.block_size) % output_fifo_capacity_;
    output_fifo_available_ += config_.block_size;
    quantum_fill_ = 0;
}

void GpuConvolutionReverb::process(const audio::BufferView<const float>& input,
                                   audio::BufferView<float>& output, std::uint32_t n) noexcept {
    if (!prepared_ || n == 0u || n > config_.block_size || input.num_channels() < 2u ||
        output.num_channels() < 2u || input.num_samples() < n || output.num_samples() < n) {
        output.clear();
        return;
    }

    // Drain and ingest in sample order.  Cache each input sample before
    // writing the output so an in-place host callback remains valid.  A newly
    // completed internal quantum is readable on the next sample, which keeps
    // the externally reported latency fixed across callback boundaries,
    // including a callback that crosses a quantum boundary.
    for (std::uint32_t i = 0; i < n; ++i) {
        const float left = std::isfinite(input.channel_ptr(0)[i]) ? input.channel_ptr(0)[i] : 0.0f;
        const float right = std::isfinite(input.channel_ptr(1)[i]) ? input.channel_ptr(1)[i] : 0.0f;

        if (output_fifo_available_ == 0u) {
            output.channel_ptr(0)[i] = 0.0f;
            output.channel_ptr(1)[i] = 0.0f;
        } else {
            output.channel_ptr(0)[i] = output_fifo_[0][output_fifo_read_];
            output.channel_ptr(1)[i] = output_fifo_[1][output_fifo_read_];
            output_fifo_read_ = (output_fifo_read_ + 1u) % output_fifo_capacity_;
            --output_fifo_available_;
        }

        const std::uint32_t fill = quantum_fill_;
        const std::uint32_t dry_capacity = 2u * config_.block_size;
        const std::uint32_t dry_index = dry_write_;
        dry_due_[0][fill] = dry_delay_[dry_index];
        dry_due_[1][fill] = dry_delay_[dry_capacity + dry_index];
        control_due_[fill] = control_delay_[dry_index];
        dry_delay_[dry_index] = left;
        dry_delay_[dry_capacity + dry_index] = right;
        control_delay_[dry_index] = ControlSample{
            wet_gain_, dry_gain_, width_, ir_gain_linear_, predelay_samples_};
        dry_write_ = (dry_write_ + 1u) % dry_capacity;

        const float source[2] = {left, right};
        for (std::size_t lane = 0; lane < 2u; ++lane) {
            double x = source[lane];
            if (hp_active_) {
                hp_z_[lane] = hp_coef_ * (hp_z_[lane] + x - hp_prev_x_[lane]);
                hp_prev_x_[lane] = x;
                x = hp_z_[lane];
            }
            if (lp_active_)
                lp_z_[lane] += lp_coef_ * (x - lp_z_[lane]);
            const float filtered = static_cast<float>(lp_active_ ? lp_z_[lane] : x);
            lanes_[lane]->input[fill] = filtered;
        }
        quantum_fill_ = fill + 1u;
        if (quantum_fill_ == config_.block_size)
            process_quantum();
    }

    for (std::size_t channel = 2; channel < output.num_channels(); ++channel)
        std::fill_n(output.channel_ptr(channel), n, 0.0f);
}

GpuConvolutionReverbReport GpuConvolutionReverb::report() const noexcept {
    GpuConvolutionReverbReport result;
    result.prepared = prepared_;
    result.gpu_enabled = config_.gpu_enabled;
    result.block_size = config_.block_size;
    result.latency_samples = latency_samples();
    result.authenticated_shared_provider = prepared_;
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
        if (!lanes_[lane] || !lanes_[lane]->transport) {
            result.authenticated_shared_provider = false;
            continue;
        }
        result.lanes[lane].capability = lanes_[lane]->transport->capability_report();
        result.lanes[lane].stats = lanes_[lane]->transport->stats();
        result.lanes[lane].delivery = lanes_[lane]->transport->delivery_snapshot();
        const auto& capability = result.lanes[lane].capability;
        result.authenticated_shared_provider &=
            capability.prepared && capability.provider == GpuAudioProvider::Dawn &&
            capability.path == GpuAudioExecutionPath::SharedMemory &&
            capability.prepared_lead_blocks == GpuConvolver::kLatencyBlocks;
    }
    return result;
}

} // namespace pulp::gpu_audio
