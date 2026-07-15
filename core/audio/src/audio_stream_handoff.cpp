#include <pulp/audio/audio_stream_handoff.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace pulp::audio {

namespace {

bool rates_match(double a, double b) noexcept {
    return std::abs(a - b) <= 1.0e-9;
}

std::uint64_t ceil_source_frames_for_host(std::uint32_t host_frames,
                                          double source_rate,
                                          double host_rate) noexcept {
    if (host_frames == 0 || source_rate <= 0.0 || host_rate <= 0.0) return 0;
    const auto frames =
        std::ceil(static_cast<double>(host_frames) * source_rate / host_rate);
    if (frames <= 0.0) return 0;
    if (frames > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(frames);
}

}  // namespace

bool AudioStreamHandoff::valid_config(const AudioStreamHandoffConfig& config) const noexcept {
    return config.source_channels > 0 && config.host_channels > 0 &&
           config.source_sample_rate > 0.0 && config.host_sample_rate > 0.0 &&
           std::isfinite(config.source_sample_rate) &&
           std::isfinite(config.host_sample_rate) &&
           config.ring_capacity_frames > 0 && config.max_source_block_frames > 0 &&
           config.max_host_block_frames > 0;
}

void AudioStreamHandoff::clear_unprepared() noexcept {
    ring_.release();
    std::vector<std::vector<float>>().swap(pending_source_);
    std::vector<std::vector<float>>().swap(resampled_output_);
    std::vector<float*>().swap(pending_write_ptrs_);
    std::vector<const float*>().swap(pending_read_ptrs_);
    std::vector<float*>().swap(resampled_output_ptrs_);
    pending_fifo_.reset();

    source_channels_ = 0;
    host_channels_ = 0;
    source_sample_rate_ = 0.0;
    host_sample_rate_ = 0.0;
    pending_capacity_frames_ = 0;
    max_host_block_frames_ = 0;
    prepared_ = false;
    same_rate_ = true;
    source_frames_pushed_.store(0, std::memory_order_relaxed);
    source_frames_consumed_.store(0, std::memory_order_relaxed);
    output_frames_pulled_.store(0, std::memory_order_relaxed);
    underrun_frames_.store(0, std::memory_order_relaxed);
    overrun_frames_.store(0, std::memory_order_relaxed);
    dropped_source_frames_.store(0, std::memory_order_relaxed);
    pending_source_frames_snapshot_.store(0, std::memory_order_relaxed);
}

void AudioStreamHandoff::allocate_scratch(std::uint64_t pending_capacity_frames) {
    if (pending_capacity_frames >
        static_cast<std::uint64_t>(std::numeric_limits<int>::max() - 1)) {
        throw std::bad_alloc();
    }
    const auto storage_frames = static_cast<std::size_t>(pending_capacity_frames + 1u);
    pending_source_.assign(source_channels_, {});
    resampled_output_.assign(source_channels_, {});
    for (auto& channel : pending_source_) {
        channel.assign(storage_frames, 0.0f);
    }
    for (auto& channel : resampled_output_) {
        channel.assign(static_cast<std::size_t>(max_host_block_frames_), 0.0f);
    }
    pending_write_ptrs_.assign(source_channels_, nullptr);
    pending_read_ptrs_.assign(source_channels_, nullptr);
    resampled_output_ptrs_.assign(source_channels_, nullptr);
    pending_fifo_ = std::make_unique<runtime::SpscRingIndex>(
        static_cast<int>(pending_capacity_frames + 1u));
}

bool AudioStreamHandoff::prepare(const AudioStreamHandoffConfig& config) {
    if (!valid_config(config)) {
        clear_unprepared();
        return false;
    }

    source_channels_ = config.source_channels;
    host_channels_ = config.host_channels;
    source_sample_rate_ = config.source_sample_rate;
    host_sample_rate_ = config.host_sample_rate;
    max_host_block_frames_ = config.max_host_block_frames;
    same_rate_ = rates_match(source_sample_rate_, host_sample_rate_);

    if (!ring_.prepare(source_channels_, config.ring_capacity_frames)) {
        clear_unprepared();
        return false;
    }

    try {
        std::uint64_t pending_capacity = config.max_source_block_frames;
        if (!same_rate_) {
            resampler_.prepare(source_sample_rate_,
                               host_sample_rate_,
                               source_channels_,
                               std::max(config.max_source_block_frames,
                                        config.max_host_block_frames));
            pending_capacity = std::max<std::uint64_t>(
                pending_capacity,
                ceil_source_frames_for_host(config.max_host_block_frames,
                                            source_sample_rate_,
                                            host_sample_rate_) +
                    static_cast<std::uint64_t>(resampler_.taps_per_phase()) + 16u);
        }
        pending_capacity_frames_ = pending_capacity;
        allocate_scratch(pending_capacity_frames_);
    } catch (...) {
        clear_unprepared();
        return false;
    }

    prepared_ = true;
    reset();
    return true;
}

void AudioStreamHandoff::reset() noexcept {
    ring_.reset();
    resampler_.reset();
    if (pending_fifo_) pending_fifo_->reset();
    source_frames_pushed_.store(0, std::memory_order_relaxed);
    source_frames_consumed_.store(0, std::memory_order_relaxed);
    output_frames_pulled_.store(0, std::memory_order_relaxed);
    underrun_frames_.store(0, std::memory_order_relaxed);
    overrun_frames_.store(0, std::memory_order_relaxed);
    dropped_source_frames_.store(0, std::memory_order_relaxed);
    pending_source_frames_snapshot_.store(0, std::memory_order_relaxed);
}

std::uint64_t AudioStreamHandoff::push(BufferView<const float> source,
                                       std::uint64_t frames) noexcept {
    return push_impl(source, frames);
}

std::uint64_t AudioStreamHandoff::push(BufferView<float> source,
                                       std::uint64_t frames) noexcept {
    return push_impl(source, frames);
}

void AudioStreamHandoff::zero_fill(BufferView<float> destination,
                                   std::uint64_t offset,
                                   std::uint64_t frames) noexcept {
    if (frames == 0) return;
    for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
        std::fill_n(destination.channel_ptr(ch) + offset,
                    static_cast<std::size_t>(frames),
                    0.0f);
    }
}

void AudioStreamHandoff::refill_pending_from_ring() noexcept {
    if (pending_capacity_frames_ == 0 || !pending_fifo_) {
        return;
    }

    while (ring_.available_frames() > 0 && pending_fifo_->free_space() > 0) {
        const auto request = static_cast<int>(std::min<std::uint64_t>(
            ring_.available_frames(),
            static_cast<std::uint64_t>(pending_fifo_->free_space())));
        int start_1 = 0;
        int size_1 = 0;
        int start_2 = 0;
        int size_2 = 0;
        pending_fifo_->prepare_to_write(request, start_1, size_1, start_2, size_2);

        auto fill_segment = [&](int start, int size) noexcept -> bool {
            if (size <= 0) return true;
            for (std::uint32_t ch = 0; ch < source_channels_; ++ch) {
                pending_write_ptrs_[ch] =
                    pending_source_[ch].data() + static_cast<std::size_t>(start);
            }
            BufferView<float> tail(pending_write_ptrs_.data(),
                                   source_channels_,
                                   static_cast<std::size_t>(size));
            const bool read = ring_.read(tail, static_cast<std::uint64_t>(size));
            pending_fifo_->finish_write(size);
            return read;
        };

        if (!fill_segment(start_1, size_1)) break;
        if (!fill_segment(start_2, size_2)) break;
        if (size_1 == 0 && size_2 == 0) break;
    }
    pending_source_frames_snapshot_.store(
        static_cast<std::uint64_t>(pending_fifo_->num_ready()),
        std::memory_order_relaxed);
}

void AudioStreamHandoff::consume_pending(std::uint64_t frames) noexcept {
    if (frames == 0 || !pending_fifo_) return;
    pending_fifo_->finish_read(static_cast<int>(frames));
    pending_source_frames_snapshot_.store(
        static_cast<std::uint64_t>(pending_fifo_->num_ready()),
        std::memory_order_relaxed);
}

AudioStreamHandoff::ResampledChunkResult AudioStreamHandoff::pull_resampled_chunk(
    BufferView<float> destination,
    std::uint64_t frames) noexcept {
    ResampledChunkResult chunk_result;
    if (!pending_fifo_) {
        return chunk_result;
    }
    while (chunk_result.output_frames < frames) {
        refill_pending_from_ring();
        if (pending_fifo_->num_ready() == 0) break;

        int start_1 = 0;
        int size_1 = 0;
        int start_2 = 0;
        int size_2 = 0;
        pending_fifo_->prepare_to_read(
            static_cast<int>(std::min<std::uint64_t>(
                pending_fifo_->num_ready(),
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()))),
            start_1, size_1, start_2, size_2);
        static_cast<void>(start_2);
        static_cast<void>(size_2);
        if (size_1 == 0) break;
        for (std::uint32_t ch = 0; ch < source_channels_; ++ch) {
            pending_read_ptrs_[ch] =
                pending_source_[ch].data() + static_cast<std::size_t>(start_1);
            resampled_output_ptrs_[ch] = resampled_output_[ch].data();
        }

        const auto result = resampler_.process_block_detailed(
            pending_read_ptrs_.data(),
            static_cast<std::size_t>(size_1),
            resampled_output_ptrs_.data(),
            static_cast<std::size_t>(frames - chunk_result.output_frames));

        if (result.input_frames_consumed > 0) {
            consume_pending(result.input_frames_consumed);
            source_frames_consumed_.fetch_add(result.input_frames_consumed,
                                              std::memory_order_relaxed);
            chunk_result.source_frames_consumed += result.input_frames_consumed;
        }

        if (result.output_frames > 0) {
            copy_resampled_to_destination(destination,
                                          chunk_result.output_frames,
                                          result.output_frames);
            chunk_result.output_frames += result.output_frames;
        }

        if (result.output_frames == 0 && result.input_frames_consumed == 0) break;
    }

    return chunk_result;
}

void AudioStreamHandoff::copy_resampled_to_destination(BufferView<float> destination,
                                                       std::uint64_t destination_offset,
                                                       std::uint64_t frames) noexcept {
    if (frames == 0) return;
    for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
        float* output = destination.channel_ptr(ch) + destination_offset;
        if (ch < source_channels_) {
            std::copy_n(resampled_output_[ch].data(), static_cast<std::size_t>(frames), output);
        } else {
            std::fill_n(output, static_cast<std::size_t>(frames), 0.0f);
        }
    }
}

AudioStreamHandoffPullResult AudioStreamHandoff::pull(BufferView<float> destination,
                                                      std::uint64_t frames) noexcept {
    AudioStreamHandoffPullResult result;
    if (!prepared_ || frames == 0 || destination.num_channels() == 0 ||
        destination.num_samples() == 0) {
        return result;
    }

    result.requested_frames =
        std::min(frames, static_cast<std::uint64_t>(destination.num_samples()));

    std::uint64_t offset = 0;
    while (offset < result.requested_frames) {
        const auto chunk =
            std::min<std::uint64_t>(result.requested_frames - offset, max_host_block_frames_);
        auto destination_chunk =
            destination.slice(static_cast<std::size_t>(offset), static_cast<std::size_t>(chunk));

        std::uint64_t source_backed = 0;
        if (same_rate_) {
            source_backed = std::min(ring_.available_frames(), chunk);
            if (source_backed > 0) {
                auto source_span = destination_chunk.slice(
                    0, static_cast<std::size_t>(source_backed));
                ring_.read(source_span, source_backed);
                source_frames_consumed_.fetch_add(source_backed, std::memory_order_relaxed);
                result.source_frames_consumed += source_backed;
            }
            if (source_backed < chunk) {
                zero_fill(destination_chunk, source_backed, chunk - source_backed);
                result.underrun = true;
            }
        } else {
            const auto resampled = pull_resampled_chunk(destination_chunk, chunk);
            source_backed = resampled.output_frames;
            result.source_frames_consumed += resampled.source_frames_consumed;
            if (source_backed < chunk) {
                zero_fill(destination_chunk, source_backed, chunk - source_backed);
                result.underrun = true;
            }
        }

        result.source_backed_frames += source_backed;
        result.rendered_frames += chunk;
        offset += chunk;
    }

    if (result.underrun) {
        underrun_frames_.fetch_add(result.rendered_frames - result.source_backed_frames,
                                   std::memory_order_relaxed);
    }
    output_frames_pulled_.fetch_add(result.rendered_frames, std::memory_order_relaxed);
    return result;
}

AudioStreamHandoffStats AudioStreamHandoff::stats() const noexcept {
    const auto queued = ring_.available_frames() +
                        pending_source_frames_snapshot_.load(std::memory_order_relaxed);
    const auto source_latency =
        resampling() ? static_cast<double>(resampler_.taps_per_phase() - 1u) * 0.5 : 0.0;
    const auto host_latency =
        source_sample_rate_ > 0.0
            ? source_latency * (host_sample_rate_ / source_sample_rate_)
            : 0.0;
    return {
        source_frames_pushed_.load(std::memory_order_relaxed),
        source_frames_consumed_.load(std::memory_order_relaxed),
        output_frames_pulled_.load(std::memory_order_relaxed),
        underrun_frames_.load(std::memory_order_relaxed),
        overrun_frames_.load(std::memory_order_relaxed),
        dropped_source_frames_.load(std::memory_order_relaxed),
        queued,
        source_sample_rate_ > 0.0 ? static_cast<double>(queued) / source_sample_rate_ : 0.0,
        source_latency,
        host_latency,
        source_sample_rate_ > 0.0 ? source_latency / source_sample_rate_ : 0.0,
    };
}

}  // namespace pulp::audio
