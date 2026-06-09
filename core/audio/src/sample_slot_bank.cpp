#include <pulp/audio/sample_slot_bank.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::audio {

namespace {

std::uint8_t encode_state(SampleSlotState state) noexcept {
    return static_cast<std::uint8_t>(state);
}

SampleSlotState decode_state(std::uint8_t state) noexcept {
    return static_cast<SampleSlotState>(state);
}

}  // namespace

bool SampleSlotBank::prepare(std::uint32_t slot_count,
                             std::uint32_t max_channels,
                             std::uint64_t max_frames_per_slot) {
    if (slot_count == 0 || max_channels == 0 || max_frames_per_slot == 0) {
        clear_unprepared();
        return false;
    }

    if (max_frames_per_slot >
        std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(max_channels)) {
        clear_unprepared();
        return false;
    }
    const auto slot_samples =
        static_cast<std::uint64_t>(max_channels) * max_frames_per_slot;
    if (slot_samples >
        std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(slot_count)) {
        clear_unprepared();
        return false;
    }
    const auto total_samples = slot_samples * static_cast<std::uint64_t>(slot_count);
    if (total_samples > std::numeric_limits<std::size_t>::max()) {
        clear_unprepared();
        return false;
    }

    try {
        storage_.assign(static_cast<std::size_t>(total_samples), 0.0f);
        states_ = std::vector<std::atomic<std::uint8_t>>(slot_count);
        write_committed_ = std::vector<std::atomic_bool>(slot_count);
        metadata_.assign(slot_count, {});
        retire_after_generation_ = std::vector<std::atomic<std::uint64_t>>(slot_count);
    } catch (...) {
        clear_unprepared();
        return false;
    }

    slot_count_ = slot_count;
    max_channels_ = max_channels;
    max_frames_per_slot_ = max_frames_per_slot;
    reset();
    return true;
}

void SampleSlotBank::clear_unprepared() noexcept {
    std::vector<float>().swap(storage_);
    std::vector<std::atomic<std::uint8_t>>().swap(states_);
    std::vector<std::atomic_bool>().swap(write_committed_);
    std::vector<SlotMetadata>().swap(metadata_);
    std::vector<std::atomic<std::uint64_t>>().swap(retire_after_generation_);
    slot_count_ = 0;
    max_channels_ = 0;
    max_frames_per_slot_ = 0;
    publish_generation_ = 0;
    current_ = {};
    published_.write({});
}

void SampleSlotBank::reset() noexcept {
    for (auto& state : states_) {
        state.store(encode_state(SampleSlotState::Free), std::memory_order_relaxed);
    }
    for (auto& committed : write_committed_) {
        committed.store(false, std::memory_order_relaxed);
    }
    for (auto& metadata : metadata_) metadata = {};
    for (auto& generation : retire_after_generation_) {
        generation.store(0, std::memory_order_relaxed);
    }
    current_ = {};
    publish_generation_ = 0;
    published_.write({});
}

void SampleSlotBank::release() noexcept {
    clear_unprepared();
}

bool SampleSlotBank::valid_slot(std::uint32_t slot_index) const noexcept {
    return slot_index < slot_count_;
}

int SampleSlotBank::reserve_slot(std::uint32_t num_channels,
                                 std::uint64_t num_frames,
                                 double sample_rate) noexcept {
    if (num_channels == 0 || num_channels > max_channels_ || num_frames == 0 ||
        num_frames > max_frames_per_slot_ || sample_rate <= 0.0 ||
        !std::isfinite(sample_rate)) {
        return -1;
    }

    for (std::uint32_t i = 0; i < slot_count_; ++i) {
        auto expected = encode_state(SampleSlotState::Free);
        if (states_[i].compare_exchange_strong(expected,
                                               encode_state(SampleSlotState::Reserved),
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            metadata_[i] = {num_channels, num_frames, sample_rate};
            write_committed_[i].store(false, std::memory_order_release);
            retire_after_generation_[i].store(0, std::memory_order_release);
            return static_cast<int>(i);
        }
    }
    return -1;
}

float* SampleSlotBank::mutable_channel_data(std::uint32_t slot_index,
                                            std::uint32_t channel) noexcept {
    const auto slot_stride =
        static_cast<std::size_t>(max_channels_) *
        static_cast<std::size_t>(max_frames_per_slot_);
    return storage_.data() + static_cast<std::size_t>(slot_index) * slot_stride +
           static_cast<std::size_t>(channel) *
               static_cast<std::size_t>(max_frames_per_slot_);
}

const float* SampleSlotBank::slot_channel_data_unchecked(std::uint32_t slot_index,
                                                         std::uint32_t channel) const noexcept {
    if (!valid_slot(slot_index) || channel >= max_channels_) return nullptr;
    const auto slot_stride =
        static_cast<std::size_t>(max_channels_) *
        static_cast<std::size_t>(max_frames_per_slot_);
    return storage_.data() + static_cast<std::size_t>(slot_index) * slot_stride +
           static_cast<std::size_t>(channel) *
               static_cast<std::size_t>(max_frames_per_slot_);
}

const float* SampleSlotBank::slot_channel_data_for_test(std::uint32_t slot_index,
                                                        std::uint32_t channel) const noexcept {
    return slot_channel_data_unchecked(slot_index, channel);
}

const float* SampleSlotBank::channel_data(std::uint32_t slot_index,
                                          std::uint32_t channel) const noexcept {
    return slot_channel_data_for_test(slot_index, channel);
}

void SampleSlotBank::clear_metadata(std::uint32_t slot_index) noexcept {
    metadata_[slot_index] = {};
    write_committed_[slot_index].store(false, std::memory_order_release);
    retire_after_generation_[slot_index].store(0, std::memory_order_release);
}

bool SampleSlotBank::write_slot(std::uint32_t slot_index,
                                BufferView<const float> source,
                                std::uint64_t frames) noexcept {
    return write_slot_impl(slot_index, source, frames);
}

bool SampleSlotBank::write_slot(std::uint32_t slot_index,
                                BufferView<float> source,
                                std::uint64_t frames) noexcept {
    return write_slot_impl(slot_index, source, frames);
}

bool SampleSlotBank::publish_from_buffer(BufferView<const float> source,
                                         std::uint64_t frames,
                                         double sample_rate,
                                         std::uint64_t audio_safe_generation) noexcept {
    return publish_from_buffer_impl(source, frames, sample_rate, audio_safe_generation);
}

bool SampleSlotBank::publish_from_buffer(BufferView<float> source,
                                         std::uint64_t frames,
                                         double sample_rate,
                                         std::uint64_t audio_safe_generation) noexcept {
    return publish_from_buffer_impl(source, frames, sample_rate, audio_safe_generation);
}

template<typename SampleType>
bool SampleSlotBank::publish_from_buffer_impl(
    BufferView<SampleType> source,
    std::uint64_t frames,
    double sample_rate,
    std::uint64_t audio_safe_generation) noexcept {
    if (frames == 0 || source.num_channels() == 0 ||
        source.num_channels() > std::numeric_limits<std::uint32_t>::max() ||
        frames > std::numeric_limits<std::size_t>::max() ||
        source.num_samples() < static_cast<std::size_t>(frames)) {
        return false;
    }

    const auto num_channels = static_cast<std::uint32_t>(source.num_channels());
    acknowledge_audio_generation(audio_safe_generation);
    int slot = reserve_slot(num_channels, frames, sample_rate);
    if (slot < 0) {
        acknowledge_audio_generation(audio_safe_generation);
        slot = reserve_slot(num_channels, frames, sample_rate);
    }
    if (slot < 0) return false;

    const auto slot_index = static_cast<std::uint32_t>(slot);
    if (!write_slot(slot_index, source, frames) ||
        !complete_slot(slot_index) ||
        !publish_slot(slot_index)) {
        release_slot(slot_index);
        return false;
    }
    return true;
}

template<typename SampleType>
bool SampleSlotBank::write_slot_impl(std::uint32_t slot_index,
                                     BufferView<SampleType> source,
                                     std::uint64_t frames) noexcept {
    if (!valid_slot(slot_index)) return false;

    const auto state = slot_state(slot_index);
    if (state != SampleSlotState::Reserved) return false;

    const auto& metadata = metadata_[slot_index];
    if (frames != metadata.num_frames ||
        static_cast<std::uint64_t>(source.num_samples()) < frames) {
        return false;
    }

    const auto source_channels = static_cast<std::uint32_t>(
        std::min<std::size_t>(source.num_channels(), metadata.num_channels));
    for (std::uint32_t ch = 0; ch < source_channels; ++ch) {
        if (source.channel_ptr(ch) == nullptr) return false;
    }

    states_[slot_index].store(encode_state(SampleSlotState::Writing),
                              std::memory_order_release);
    for (std::uint32_t ch = 0; ch < metadata.num_channels; ++ch) {
        float* destination = mutable_channel_data(slot_index, ch);
        if (ch < source.num_channels()) {
            const SampleType* source_ptr = source.channel_ptr(ch);
            std::copy_n(source_ptr, static_cast<std::size_t>(frames), destination);
        } else {
            std::fill_n(destination, static_cast<std::size_t>(frames), 0.0f);
        }
    }

    write_committed_[slot_index].store(true, std::memory_order_release);
    return true;
}

bool SampleSlotBank::complete_slot(std::uint32_t slot_index) noexcept {
    if (!valid_slot(slot_index)) return false;
    const auto state = slot_state(slot_index);
    if (state != SampleSlotState::Writing ||
        !write_committed_[slot_index].load(std::memory_order_acquire)) {
        return false;
    }

    states_[slot_index].store(encode_state(SampleSlotState::Completed),
                              std::memory_order_release);
    return true;
}

bool SampleSlotBank::publish_slot(std::uint32_t slot_index) noexcept {
    if (!valid_slot(slot_index) || slot_state(slot_index) != SampleSlotState::Completed) {
        return false;
    }

    const auto new_generation = publish_generation_ + 1;
    const auto previous = current_;

    const auto& metadata = metadata_[slot_index];
    current_ = {
        true,
        slot_index,
        new_generation,
        metadata.num_channels,
        metadata.num_frames,
        metadata.sample_rate,
    };
    publish_generation_ = new_generation;

    if (previous.valid && previous.slot_index != slot_index &&
        slot_state(previous.slot_index) == SampleSlotState::Published) {
        retire_after_generation_[previous.slot_index].store(new_generation,
                                                            std::memory_order_release);
        states_[previous.slot_index].store(encode_state(SampleSlotState::Retired),
                                           std::memory_order_release);
    }

    states_[slot_index].store(encode_state(SampleSlotState::Published),
                              std::memory_order_release);
    published_.write(current_);
    return true;
}

bool SampleSlotBank::release_slot(std::uint32_t slot_index) noexcept {
    if (!valid_slot(slot_index)) return false;
    const auto state = slot_state(slot_index);
    if (state == SampleSlotState::Published || state == SampleSlotState::Retired) {
        return false;
    }

    clear_metadata(slot_index);
    states_[slot_index].store(encode_state(SampleSlotState::Free),
                              std::memory_order_release);
    return true;
}

bool SampleSlotBank::acknowledge_audio_generation(std::uint64_t generation) noexcept {
    bool freed_any = false;
    for (std::uint32_t i = 0; i < slot_count_; ++i) {
        if (slot_state(i) != SampleSlotState::Retired) continue;
        const auto retire_after =
            retire_after_generation_[i].load(std::memory_order_acquire);
        if (retire_after == 0 || generation < retire_after) continue;

        clear_metadata(i);
        states_[i].store(encode_state(SampleSlotState::Free), std::memory_order_release);
        freed_any = true;
    }
    return freed_any;
}

SampleSlotState SampleSlotBank::slot_state(std::uint32_t slot_index) const noexcept {
    if (!valid_slot(slot_index)) return SampleSlotState::Free;
    return decode_state(states_[slot_index].load(std::memory_order_acquire));
}

std::uint32_t SampleSlotBank::slot_num_channels(std::uint32_t slot_index) const noexcept {
    return valid_slot(slot_index) ? metadata_[slot_index].num_channels : 0;
}

std::uint64_t SampleSlotBank::slot_num_frames(std::uint32_t slot_index) const noexcept {
    return valid_slot(slot_index) ? metadata_[slot_index].num_frames : 0;
}

double SampleSlotBank::slot_sample_rate(std::uint32_t slot_index) const noexcept {
    return valid_slot(slot_index) ? metadata_[slot_index].sample_rate : 0.0;
}

bool SampleSlotBank::slot_view_valid(const PublishedSampleView& view) const noexcept {
    if (!view.valid || !valid_slot(view.slot_index)) return false;

    const auto state = slot_state(view.slot_index);
    if (state != SampleSlotState::Published && state != SampleSlotState::Retired) {
        return false;
    }

    const auto& metadata = metadata_[view.slot_index];
    if (metadata.num_channels != view.num_channels ||
        metadata.num_frames != view.num_frames ||
        metadata.sample_rate != view.sample_rate) {
        return false;
    }

    if (state == SampleSlotState::Published) {
        const auto published = published_.read();
        return published.valid && published.slot_index == view.slot_index &&
               published.generation == view.generation;
    }

    const auto retire_after =
        retire_after_generation_[view.slot_index].load(std::memory_order_acquire);
    return retire_after != 0 && view.generation < retire_after;
}

const float* SampleSlotBank::channel_data(const PublishedSampleView& view,
                                          std::uint32_t channel) const noexcept {
    if (channel >= view.num_channels || !slot_view_valid(view)) return nullptr;
    return slot_channel_data_unchecked(view.slot_index, channel);
}

std::uint64_t SampleSlotBank::oldest_active_generation(
    const PublishedSampleView& current,
    const PublishedSampleView* active_views,
    std::size_t active_count) noexcept {
    std::uint64_t generation = current.valid ? current.generation : 0;
    bool have_generation = current.valid;

    if (active_views != nullptr) {
        for (std::size_t i = 0; i < active_count; ++i) {
            const auto& view = active_views[i];
            if (!view.valid) continue;
            generation = have_generation ? std::min(generation, view.generation)
                                         : view.generation;
            have_generation = true;
        }
    }

    return have_generation ? generation : 0;
}

template bool SampleSlotBank::write_slot_impl<const float>(std::uint32_t,
                                                           BufferView<const float>,
                                                           std::uint64_t) noexcept;
template bool SampleSlotBank::write_slot_impl<float>(std::uint32_t,
                                                     BufferView<float>,
                                                     std::uint64_t) noexcept;
template bool SampleSlotBank::publish_from_buffer_impl<const float>(
    BufferView<const float>,
    std::uint64_t,
    double,
    std::uint64_t) noexcept;
template bool SampleSlotBank::publish_from_buffer_impl<float>(
    BufferView<float>,
    std::uint64_t,
    double,
    std::uint64_t) noexcept;

}  // namespace pulp::audio
