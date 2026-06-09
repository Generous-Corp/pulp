#include <pulp/audio/sample_stream_window.hpp>

#include <algorithm>
#include <limits>

namespace pulp::audio {

namespace {

std::uint8_t encode_state(SampleStreamPageState state) noexcept {
    return static_cast<std::uint8_t>(state);
}

SampleStreamPageState decode_state(std::uint8_t state) noexcept {
    return static_cast<SampleStreamPageState>(state);
}

bool add_overflows(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

bool ranges_overlap(std::uint64_t a_start,
                    std::uint64_t a_frames,
                    std::uint64_t b_start,
                    std::uint64_t b_frames) noexcept {
    const auto a_end = a_start + a_frames;
    const auto b_end = b_start + b_frames;
    return a_start < b_end && b_start < a_end;
}

}  // namespace

bool SampleStreamWindow::prepare(const SampleStreamWindowConfig& config) {
    if (config.channels == 0 || config.page_count == 0 || config.page_frames == 0) {
        clear_unprepared();
        return false;
    }
    if (config.page_frames > std::numeric_limits<std::size_t>::max()) {
        clear_unprepared();
        return false;
    }

    const auto channels = static_cast<std::uint64_t>(config.channels);
    const auto pages = static_cast<std::uint64_t>(config.page_count);
    if (config.page_frames >
        std::numeric_limits<std::uint64_t>::max() / channels) {
        clear_unprepared();
        return false;
    }
    const auto page_samples = config.page_frames * channels;
    if (page_samples > std::numeric_limits<std::uint64_t>::max() / pages) {
        clear_unprepared();
        return false;
    }
    const auto total_samples = page_samples * pages;
    if (total_samples > std::numeric_limits<std::size_t>::max()) {
        clear_unprepared();
        return false;
    }

    try {
        storage_.assign(static_cast<std::size_t>(total_samples), 0.0f);
        states_ = std::vector<std::atomic<std::uint8_t>>(config.page_count);
        metadata_.assign(config.page_count, {});
        retire_after_generation_ =
            std::vector<std::atomic<std::uint64_t>>(config.page_count);
    } catch (...) {
        clear_unprepared();
        return false;
    }

    channels_ = config.channels;
    page_count_ = config.page_count;
    page_frames_ = config.page_frames;
    prepared_ = true;
    reset_when_quiescent();
    return true;
}

void SampleStreamWindow::release() noexcept {
    clear_unprepared();
}

void SampleStreamWindow::reset_when_quiescent() noexcept {
    if (!prepared_) return;

    std::fill(storage_.begin(), storage_.end(), 0.0f);
    for (auto& state : states_) {
        state.store(encode_state(SampleStreamPageState::Empty),
                    std::memory_order_relaxed);
    }
    for (auto& metadata : metadata_) metadata = {};
    for (auto& generation : retire_after_generation_) {
        generation.store(0, std::memory_order_relaxed);
    }

    pages_published_.store(0, std::memory_order_relaxed);
    pages_retired_.store(0, std::memory_order_relaxed);
    ready_read_chunks_.store(0, std::memory_order_relaxed);
    ready_frames_read_.store(0, std::memory_order_relaxed);
    missed_frames_.store(0, std::memory_order_relaxed);
    zero_filled_frames_.store(0, std::memory_order_relaxed);
}

void SampleStreamWindow::clear_unprepared() noexcept {
    std::vector<float>().swap(storage_);
    std::vector<std::atomic<std::uint8_t>>().swap(states_);
    std::vector<PageMetadata>().swap(metadata_);
    std::vector<std::atomic<std::uint64_t>>().swap(retire_after_generation_);

    channels_ = 0;
    page_count_ = 0;
    page_frames_ = 0;
    prepared_ = false;
    pages_published_.store(0, std::memory_order_relaxed);
    pages_retired_.store(0, std::memory_order_relaxed);
    ready_read_chunks_.store(0, std::memory_order_relaxed);
    ready_frames_read_.store(0, std::memory_order_relaxed);
    missed_frames_.store(0, std::memory_order_relaxed);
    zero_filled_frames_.store(0, std::memory_order_relaxed);
}

bool SampleStreamWindow::valid_page(std::uint32_t page_index) const noexcept {
    return prepared_ && page_index < page_count_;
}

SampleStreamPageState SampleStreamWindow::page_state(
    std::uint32_t page_index) const noexcept {
    if (!valid_page(page_index)) return SampleStreamPageState::Empty;
    return decode_state(states_[page_index].load(std::memory_order_acquire));
}

float* SampleStreamWindow::mutable_page_channel(std::uint32_t page_index,
                                                std::uint32_t channel) noexcept {
    if (!valid_page(page_index) || channel >= channels_) return nullptr;
    const auto page_stride =
        static_cast<std::size_t>(channels_) * static_cast<std::size_t>(page_frames_);
    return storage_.data() + static_cast<std::size_t>(page_index) * page_stride +
           static_cast<std::size_t>(channel) * static_cast<std::size_t>(page_frames_);
}

const float* SampleStreamWindow::page_channel(std::uint32_t page_index,
                                              std::uint32_t channel) const noexcept {
    if (!valid_page(page_index) || channel >= channels_) return nullptr;
    const auto page_stride =
        static_cast<std::size_t>(channels_) * static_cast<std::size_t>(page_frames_);
    return storage_.data() + static_cast<std::size_t>(page_index) * page_stride +
           static_cast<std::size_t>(channel) * static_cast<std::size_t>(page_frames_);
}

void SampleStreamWindow::clear_page(std::uint32_t page_index) noexcept {
    if (!valid_page(page_index)) return;
    const auto page_stride =
        static_cast<std::size_t>(channels_) * static_cast<std::size_t>(page_frames_);
    auto* page = storage_.data() + static_cast<std::size_t>(page_index) * page_stride;
    std::fill_n(page, page_stride, 0.0f);
    metadata_[page_index] = {};
    retire_after_generation_[page_index].store(0, std::memory_order_release);
}

bool SampleStreamWindow::begin_fill_page(
    std::uint32_t page_index,
    std::uint64_t completed_audio_generation) noexcept {
    if (!valid_page(page_index)) return false;

    auto expected = encode_state(SampleStreamPageState::Empty);
    if (states_[page_index].compare_exchange_strong(
            expected,
            encode_state(SampleStreamPageState::Filling),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        clear_page(page_index);
        return true;
    }

    const auto state = states_[page_index].load(std::memory_order_acquire);
    if (state != encode_state(SampleStreamPageState::Retired)) return false;

    const auto retire_after =
        retire_after_generation_[page_index].load(std::memory_order_acquire);
    if (completed_audio_generation < retire_after) return false;

    expected = encode_state(SampleStreamPageState::Retired);
    if (!states_[page_index].compare_exchange_strong(
            expected,
            encode_state(SampleStreamPageState::Filling),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return false;
    }
    clear_page(page_index);
    return true;
}

float* SampleStreamWindow::writable_channel_data(std::uint32_t page_index,
                                                 std::uint32_t channel) noexcept {
    if (page_state(page_index) != SampleStreamPageState::Filling) return nullptr;
    return mutable_page_channel(page_index, channel);
}

template<typename SampleType>
bool SampleStreamWindow::copy_to_filling_page_impl(
    std::uint32_t page_index,
    BufferView<SampleType> source,
    std::uint64_t frames) noexcept {
    if (page_state(page_index) != SampleStreamPageState::Filling) return false;
    if (frames == 0 || frames > page_frames_ ||
        frames > static_cast<std::uint64_t>(source.num_samples()) ||
        source.num_channels() != channels_) {
        return false;
    }

    const auto frame_count = static_cast<std::size_t>(frames);
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
        auto* destination = mutable_page_channel(page_index, channel);
        if (destination == nullptr) return false;

        if (channel < source.num_channels()) {
            const auto* input = source.channel_ptr(channel);
            std::copy_n(input, frame_count, destination);
        } else {
            std::fill_n(destination, frame_count, 0.0f);
        }
    }
    return true;
}

bool SampleStreamWindow::copy_to_filling_page(
    std::uint32_t page_index,
    BufferView<const float> source,
    std::uint64_t frames) noexcept {
    return copy_to_filling_page_impl(page_index, source, frames);
}

bool SampleStreamWindow::copy_to_filling_page(std::uint32_t page_index,
                                              BufferView<float> source,
                                              std::uint64_t frames) noexcept {
    return copy_to_filling_page_impl(page_index, source, frames);
}

bool SampleStreamWindow::descriptor_valid(
    const SampleStreamPageDescriptor& descriptor) const noexcept {
    return descriptor.stream_generation != 0 && descriptor.valid_frames > 0 &&
           descriptor.valid_frames <= page_frames_ &&
           !add_overflows(descriptor.start_frame, descriptor.valid_frames);
}

bool SampleStreamWindow::overlaps_ready_page(
    std::uint32_t page_index,
    const SampleStreamPageDescriptor& descriptor) const noexcept {
    for (std::uint32_t index = 0; index < page_count_; ++index) {
        if (index == page_index) continue;
        if (decode_state(states_[index].load(std::memory_order_acquire)) !=
            SampleStreamPageState::Ready) {
            continue;
        }

        const auto& other = metadata_[index].descriptor;
        if (other.stream_generation != descriptor.stream_generation) continue;
        if (ranges_overlap(descriptor.start_frame,
                           descriptor.valid_frames,
                           other.start_frame,
                           other.valid_frames)) {
            return true;
        }
    }
    return false;
}

bool SampleStreamWindow::publish_page(
    std::uint32_t page_index,
    const SampleStreamPageDescriptor& descriptor) noexcept {
    if (page_state(page_index) != SampleStreamPageState::Filling) return false;
    if (!descriptor_valid(descriptor)) return false;
    if (overlaps_ready_page(page_index, descriptor)) return false;

    metadata_[page_index].descriptor = descriptor;
    states_[page_index].store(encode_state(SampleStreamPageState::Ready),
                              std::memory_order_release);
    pages_published_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SampleStreamWindow::cancel_fill_page(std::uint32_t page_index) noexcept {
    if (!valid_page(page_index)) return false;
    auto expected = encode_state(SampleStreamPageState::Filling);
    if (!states_[page_index].compare_exchange_strong(
            expected,
            encode_state(SampleStreamPageState::Empty),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return false;
    }
    clear_page(page_index);
    return true;
}

bool SampleStreamWindow::retire_page(
    std::uint32_t page_index,
    std::uint64_t retire_after_audio_generation) noexcept {
    if (!valid_page(page_index)) return false;
    auto expected = encode_state(SampleStreamPageState::Ready);
    if (!states_[page_index].compare_exchange_strong(
            expected,
            encode_state(SampleStreamPageState::Retiring),
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return false;
    }

    retire_after_generation_[page_index].store(retire_after_audio_generation,
                                               std::memory_order_release);
    states_[page_index].store(
            encode_state(SampleStreamPageState::Retired),
            std::memory_order_release);
    pages_retired_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

SampleStreamPageView SampleStreamWindow::ready_page_for_frame(
    std::uint64_t stream_generation,
    std::uint64_t source_frame) const noexcept {
    SampleStreamPageView view;
    if (!prepared_ || stream_generation == 0) return view;

    for (std::uint32_t index = 0; index < page_count_; ++index) {
        if (decode_state(states_[index].load(std::memory_order_acquire)) !=
            SampleStreamPageState::Ready) {
            continue;
        }

        const auto& descriptor = metadata_[index].descriptor;
        if (descriptor.stream_generation != stream_generation) continue;
        if (source_frame < descriptor.start_frame) continue;
        const auto local_offset = source_frame - descriptor.start_frame;
        if (local_offset >= descriptor.valid_frames) continue;

        view.valid = true;
        view.page_index = index;
        view.stream_generation = stream_generation;
        view.start_frame = descriptor.start_frame;
        view.valid_frames = descriptor.valid_frames;
        view.local_offset = local_offset;
        view.final_page = descriptor.final_page;
        return view;
    }
    return view;
}

const float* SampleStreamWindow::ready_channel_data(
    const SampleStreamPageView& view,
    std::uint32_t channel) const noexcept {
    if (!view.valid || channel >= channels_ || view.local_offset >= page_frames_) {
        return nullptr;
    }
    if (!valid_page(view.page_index)) return nullptr;
    const auto state = decode_state(
        states_[view.page_index].load(std::memory_order_acquire));
    if (state != SampleStreamPageState::Ready &&
        state != SampleStreamPageState::Retiring &&
        state != SampleStreamPageState::Retired) {
        return nullptr;
    }
    const auto& descriptor = metadata_[view.page_index].descriptor;
    if (descriptor.stream_generation != view.stream_generation ||
        descriptor.start_frame != view.start_frame ||
        descriptor.valid_frames != view.valid_frames ||
        descriptor.final_page != view.final_page ||
        view.local_offset >= descriptor.valid_frames) {
        return nullptr;
    }
    const auto* base = page_channel(view.page_index, channel);
    if (base == nullptr) return nullptr;
    return base + static_cast<std::size_t>(view.local_offset);
}

std::uint64_t SampleStreamWindow::frames_until_next_ready_page(
    std::uint64_t stream_generation,
    std::uint64_t source_frame,
    std::uint64_t limit) const noexcept {
    if (limit == 0 || stream_generation == 0) return limit;

    std::uint64_t nearest_gap = limit;
    for (std::uint32_t index = 0; index < page_count_; ++index) {
        if (decode_state(states_[index].load(std::memory_order_acquire)) !=
            SampleStreamPageState::Ready) {
            continue;
        }

        const auto& descriptor = metadata_[index].descriptor;
        if (descriptor.stream_generation != stream_generation) continue;
        if (descriptor.start_frame <= source_frame) continue;
        const auto gap = descriptor.start_frame - source_frame;
        nearest_gap = std::min(nearest_gap, gap);
    }
    return std::max<std::uint64_t>(nearest_gap, 1);
}

SampleStreamWindowReadResult SampleStreamWindow::read_frames(
    BufferView<float> destination,
    const SampleStreamWindowReadRequest& request) noexcept {
    SampleStreamWindowReadResult result;
    if (!prepared_ || destination.empty() || request.frames == 0) return result;

    result.requested_frames = std::min<std::uint64_t>(
        request.frames,
        static_cast<std::uint64_t>(destination.num_samples()));

    std::uint64_t offset = 0;
    while (offset < result.requested_frames) {
        if (add_overflows(request.start_frame, offset)) {
            const auto remaining = result.requested_frames - offset;
            result.missed_frames += remaining;
            if (request.zero_fill_misses && !request.accumulate) {
                for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
                    std::fill_n(destination.channel_ptr(channel) + offset,
                                static_cast<std::size_t>(remaining),
                                0.0f);
                }
                result.zero_filled_frames += remaining;
            }
            break;
        }

        const auto source_frame = request.start_frame + offset;
        const auto view = ready_page_for_frame(request.stream_generation, source_frame);
        if (!view.valid) {
            const auto miss_frames = frames_until_next_ready_page(
                request.stream_generation,
                source_frame,
                result.requested_frames - offset);
            if (request.zero_fill_misses && !request.accumulate) {
                for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
                    std::fill_n(destination.channel_ptr(channel) + offset,
                                static_cast<std::size_t>(miss_frames),
                                0.0f);
                }
                result.zero_filled_frames += miss_frames;
            }
            result.missed_frames += miss_frames;
            offset += miss_frames;
            continue;
        }

        const auto chunk = std::min<std::uint64_t>(
            result.requested_frames - offset,
            view.valid_frames - view.local_offset);
        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            const float* source = nullptr;
            if (channels_ == 1) {
                source = ready_channel_data(view, 0);
            } else if (channel < channels_) {
                source = ready_channel_data(view, static_cast<std::uint32_t>(channel));
            }

            auto* output = destination.channel_ptr(channel) + offset;
            if (source != nullptr) {
                if (request.accumulate) {
                    for (std::uint64_t frame = 0; frame < chunk; ++frame) {
                        output[frame] += source[frame];
                    }
                } else {
                    std::copy_n(source, static_cast<std::size_t>(chunk), output);
                }
            } else if (!request.accumulate) {
                std::fill_n(output, static_cast<std::size_t>(chunk), 0.0f);
            }
        }

        result.copied_frames += chunk;
        ready_read_chunks_.fetch_add(1, std::memory_order_relaxed);
        offset += chunk;
    }

    result.complete =
        result.requested_frames == result.copied_frames && result.missed_frames == 0;
    ready_frames_read_.fetch_add(result.copied_frames, std::memory_order_relaxed);
    missed_frames_.fetch_add(result.missed_frames, std::memory_order_relaxed);
    zero_filled_frames_.fetch_add(result.zero_filled_frames, std::memory_order_relaxed);
    return result;
}

SampleStreamWindowStats SampleStreamWindow::stats() const noexcept {
    std::uint32_t ready_pages = 0;
    for (std::uint32_t index = 0; index < page_count_; ++index) {
        if (decode_state(states_[index].load(std::memory_order_acquire)) ==
            SampleStreamPageState::Ready) {
            ++ready_pages;
        }
    }

    return {
        pages_published_.load(std::memory_order_relaxed),
        pages_retired_.load(std::memory_order_relaxed),
        ready_read_chunks_.load(std::memory_order_relaxed),
        ready_frames_read_.load(std::memory_order_relaxed),
        missed_frames_.load(std::memory_order_relaxed),
        zero_filled_frames_.load(std::memory_order_relaxed),
        ready_pages,
    };
}

}  // namespace pulp::audio
