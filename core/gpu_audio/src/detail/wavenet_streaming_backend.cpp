#include "wavenet_streaming_backend.hpp"

#include <algorithm>
#include <chrono>

namespace pulp::gpu_audio::detail {
namespace {

class SessionChannel final : public WaveNetRealtimeChannel {
  public:
    explicit SessionChannel(std::unique_ptr<GpuWaveNetSession> session)
        : session_(std::move(session)) {}
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        return session_->submit_block(input, sequence);
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t, std::uint64_t deadline) noexcept override {
        session_->service_until(deadline);
    }
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept override {
        return session_->receive(output);
    }
    bool release() noexcept override {
        return session_->release();
    }

  private:
    std::unique_ptr<GpuWaveNetSession> session_;
};

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

WaveNetStreamingBackend::WaveNetStreamingBackend(const Config& config, ChannelFactory factory)
    : config_(config), factory_(std::move(factory)), production_(!factory_),
      layers_(config.session.descriptor.layers.begin(), config.session.descriptor.layers.end()),
      weights_(config.session.weights.begin(), config.session.weights.end()) {
    dilations_.reserve(layers_.size());
    for (auto& layer : layers_) {
        dilations_.emplace_back(layer.dilations.begin(), layer.dilations.end());
        layer.dilations = dilations_.back();
    }
    config_.session.descriptor.layers = layers_;
    config_.session.weights = weights_;
}

WaveNetStreamingBackend::~WaveNetStreamingBackend() {
    (void)release();
}

bool WaveNetStreamingBackend::prepare(const StreamingPrepareContext& context) noexcept {
    const auto& c = config_;
    const auto& descriptor = c.session.descriptor;
    if (prepared_ || !channels_.empty() || !valid_streaming_prepare_context(context) ||
        c.channels == 0 || c.channels > 64 || c.max_inflight == 0 ||
        c.max_inflight > c.session.slots || c.session.slots < 2 ||
        !validate_gpu_wavenet_descriptor(descriptor).accepted() ||
        weights_.size() != descriptor.weight_count ||
        context.spec->block_size != descriptor.block_size ||
        context.spec->sample_rate != descriptor.sample_rate ||
        context.spec->input_channels != c.channels || context.spec->output_channels != c.channels)
        return false;
    try {
        for (std::uint32_t ch = 0; ch < c.channels; ++ch) {
            std::unique_ptr<WaveNetRealtimeChannel> channel;
            if (factory_) {
                channel = factory_(ch);
            } else {
                auto result = GpuWaveNetSession::create(c.session);
                // Retain a partially prepared session so release() can retry
                // its physical drain.
                if (result.session)
                    channels_.push_back(
                        std::make_unique<SessionChannel>(std::move(result.session)));
                if (!result) {
                    (void)release();
                    return false;
                }
                continue;
            }
            if (!channel) {
                (void)release();
                return false;
            }
            channels_.push_back(std::move(channel));
        }
        slots_.assign(c.max_inflight, {});
        scratch_.assign(descriptor.block_size, 0.0f);
    } catch (...) {
        (void)release();
        return false;
    }
    block_size_ = descriptor.block_size;
    all_channels_mask_ = c.channels == 64 ? UINT64_MAX : (UINT64_C(1) << c.channels) - 1;
    head_ = count_ = 0;
    epoch_ = 0;
    prepared_ = true;
    return true;
}

StreamingAdmission
WaveNetStreamingBackend::enqueue_validated(const StreamingBlock& block) noexcept {
    if (!prepared_ || count_ == slots_.size() || block.stamp.epoch == 0 ||
        block.stamp.epoch != epoch_ || block.input_channels != config_.channels ||
        block.output_channels != config_.channels || block.frames != block_size_)
        return StreamingAdmission::Rejected;
    auto& slot = slots_[(head_ + count_) % slots_.size()];
    slot = {};
    slot.stamp = block.stamp;
    slot.output = block.output;
    slot.output_channel_stride = block.output_channel_stride;
    slot.output_frame_stride = block.output_frame_stride;
    slot.occupied = true;
    std::uint32_t submitted = 0;
    for (std::uint32_t ch = 0; ch < config_.channels; ++ch) {
        std::span<const float> samples;
        const auto base = static_cast<std::size_t>(ch) * block.input_channel_stride;
        if (block.input_frame_stride == 1) {
            samples = block.input.subspan(base, block.frames);
        } else {
            for (std::uint32_t i = 0; i < block.frames; ++i)
                scratch_[i] =
                    block.input[base + static_cast<std::size_t>(i) * block.input_frame_stride];
            samples = {scratch_.data(), block.frames};
        }
        if (channels_[ch]->submit(samples, block.stamp.sequence)) {
            ++submitted;
        } else {
            // The channel holds no work for this sequence; account for it now
            // so the remaining channels still retire the block as a failure.
            slot.completed_mask |= UINT64_C(1) << ch;
            slot.failed = true;
        }
    }
    if (submitted == 0) {
        slot = {};
        return StreamingAdmission::Rejected;
    }
    ++count_;
    return StreamingAdmission::Accepted;
}

WaveNetStreamingBackend::Slot* WaveNetStreamingBackend::find(std::uint64_t sequence) noexcept {
    for (std::size_t offset = 0; offset < count_; ++offset) {
        auto& slot = slots_[(head_ + offset) % slots_.size()];
        if (slot.occupied && slot.stamp.sequence == sequence)
            return &slot;
    }
    return nullptr;
}

void WaveNetStreamingBackend::fail_all_pending() noexcept {
    for (std::size_t offset = 0; offset < count_; ++offset)
        slots_[(head_ + offset) % slots_.size()].failed = true;
}

std::size_t WaveNetStreamingBackend::service_until(std::uint64_t deadline_ns) noexcept {
    if (!prepared_)
        return 0;
    const auto now = now_ns();
    for (std::uint32_t ch = 0; ch < channels_.size(); ++ch) {
        if (deadline_ns > now)
            channels_[ch]->service_until(now, deadline_ns);
        else
            channels_[ch]->service(now);
        for (std::size_t received = 0; received < count_; ++received) {
            auto result = channels_[ch]->receive(scratch_);
            if (!result)
                break;
            auto* slot = find(result->sequence);
            if (slot == nullptr) {
                // An unattributable completion means this provider stream is
                // no longer trustworthy; nothing pending may be delivered.
                fail_all_pending();
                continue;
            }
            const auto bit = UINT64_C(1) << ch;
            if ((slot->completed_mask & bit) != 0) {
                slot->failed = true;
                continue;
            }
            slot->completed_mask |= bit;
            slot->late = slot->late || result->late;
            if (result->status != GpuWaveNetBlockStatus::GpuDelivered) {
                slot->failed = true;
                continue;
            }
            const auto base = static_cast<std::size_t>(ch) * slot->output_channel_stride;
            for (std::uint32_t i = 0; i < block_size_; ++i)
                slot->output[base + static_cast<std::size_t>(i) * slot->output_frame_stride] =
                    scratch_[i];
        }
    }
    std::size_t ready = 0;
    for (std::size_t offset = 0; offset < count_; ++offset) {
        if (slots_[(head_ + offset) % slots_.size()].completed_mask != all_channels_mask_)
            break;
        ++ready;
    }
    return ready;
}

bool WaveNetStreamingBackend::receive(StreamingTerminal& terminal) noexcept {
    if (count_ == 0)
        return false;
    auto& slot = slots_[head_];
    if (slot.completed_mask != all_channels_mask_)
        return false;
    terminal.stamp = slot.stamp;
    if (slot.stamp.epoch != epoch_ || slot.late)
        terminal.disposition = StreamingBackendTerminalDisposition::Stale;
    else if (slot.failed)
        terminal.disposition = StreamingBackendTerminalDisposition::ProviderFailed;
    else
        terminal.disposition = StreamingBackendTerminalDisposition::Completed;
    slot = {};
    head_ = (head_ + 1) % slots_.size();
    --count_;
    return true;
}

bool WaveNetStreamingBackend::begin_epoch(std::uint64_t epoch, StreamingResetReason) noexcept {
    // Sessions cannot cancel submitted work, so an epoch boundary requires
    // the owner to have retired every admitted block first.
    if (!prepared_ || count_ != 0 || epoch <= epoch_)
        return false;
    epoch_ = epoch;
    return true;
}

bool WaveNetStreamingBackend::release_channels() noexcept {
    bool drained = true;
    for (auto& channel : channels_)
        if (!channel->release())
            drained = false;
    return drained;
}

bool WaveNetStreamingBackend::quiesce() noexcept {
    // Physical drain: the provider retires or abandons all submitted slots.
    // A failed drain keeps the channel owners for a later retry.
    if (!release_channels())
        return false;
    prepared_ = false;
    for (auto& slot : slots_)
        slot = {};
    head_ = count_ = 0;
    return true;
}

bool WaveNetStreamingBackend::release() noexcept {
    if (!quiesce())
        return false;
    channels_.clear();
    slots_.clear();
    scratch_.clear();
    epoch_ = 0;
    return true;
}

} // namespace pulp::gpu_audio::detail
