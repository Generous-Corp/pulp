#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_converter.hpp>

#include <cmath>
#include <limits>

namespace pulp::audio {

bool PublishedSampleStore::config_valid(
    const PublishedSampleStoreConfig& config) const noexcept {
    return config.slot_count >= 2 &&
           config.max_channels > 0 &&
           config.max_frames_per_slot > 0;
}

bool PublishedSampleStore::prepare(const PublishedSampleStoreConfig& config) {
    if (!config_valid(config)) {
        release();
        return false;
    }
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (!ensure_capacity(config)) return false;
    storage_ready_ = true;
    return true;
}

void PublishedSampleStore::release() noexcept {
    std::lock_guard<std::mutex> lock(load_mutex_);
    slots_.release();
    std::vector<float>().swap(load_scratch_);
    config_ = {};
    storage_ready_ = false;
}

bool PublishedSampleStore::ensure_capacity(
    const PublishedSampleStoreConfig& config) {
    if (!config_valid(config)) return false;
    if (slots_.slot_count() != 0) {
        if (slots_.slot_count() >= config.slot_count &&
            slots_.max_channels() >= config.max_channels &&
            slots_.max_frames_per_slot() >= config.max_frames_per_slot) {
            slots_.reset();
            config_ = config;
            return true;
        }
        return false;
    }
    if (!slots_.prepare(config.slot_count,
                        config.max_channels,
                        config.max_frames_per_slot)) {
        return false;
    }
    config_ = config;
    return true;
}

bool PublishedSampleStore::load_mono(const float* data,
                                     int num_samples,
                                     double sample_rate,
                                     std::uint64_t audio_safe_generation) {
    if (data == nullptr || num_samples <= 0) return false;
    std::lock_guard<std::mutex> lock(load_mutex_);
    const auto frame_count = static_cast<std::uint64_t>(num_samples);
    const float* channels[] = {data};
    BufferView<const float> view(channels, 1, static_cast<std::size_t>(frame_count));
    return publish_locked(view, frame_count, sample_rate, audio_safe_generation);
}

bool PublishedSampleStore::load_interleaved_stereo(
    const float* interleaved,
    int num_frames,
    double sample_rate,
    std::uint64_t audio_safe_generation) {
    if (interleaved == nullptr || num_frames <= 0) return false;
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (!storage_ready_ || config_.max_channels < 2 ||
        !(sample_rate > 0.0) || !std::isfinite(sample_rate)) {
        return false;
    }

    const auto frame_count = static_cast<std::uint64_t>(num_frames);
    if (frame_count > config_.max_frames_per_slot ||
        frame_count > std::numeric_limits<std::size_t>::max() / 2u) {
        return false;
    }

    const auto frames = static_cast<std::size_t>(frame_count);
    try {
        load_scratch_.resize(frames * 2u);
    } catch (...) {
        return false;
    }

    float* left = load_scratch_.data();
    float* right = load_scratch_.data() + frames;
    float* channels[] = {left, right};
    SampleConverter converter(SampleFormat::Float32,
                              Endianness::Native,
                              Layout::Interleaved);
    converter.to_float(interleaved, channels, frames, 2);

    const float* const_channels[] = {left, right};
    BufferView<const float> view(const_channels, 2, frames);
    return publish_locked(view, frame_count, sample_rate, audio_safe_generation);
}

bool PublishedSampleStore::publish(BufferView<const float> source,
                                   std::uint64_t frames,
                                   double sample_rate,
                                   std::uint64_t audio_safe_generation) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return publish_locked(source, frames, sample_rate, audio_safe_generation);
}

bool PublishedSampleStore::publish_locked(BufferView<const float> source,
                                          std::uint64_t frames,
                                          double sample_rate,
                                          std::uint64_t audio_safe_generation) {
    if (!storage_ready_ ||
        source.num_channels() == 0 ||
        source.num_channels() > config_.max_channels ||
        frames == 0 ||
        frames > config_.max_frames_per_slot ||
        frames > std::numeric_limits<std::size_t>::max() ||
        source.num_samples() < static_cast<std::size_t>(frames) ||
        !(sample_rate > 0.0) ||
        !std::isfinite(sample_rate)) {
        return false;
    }
    return slots_.publish_from_buffer(source,
                                      frames,
                                      sample_rate,
                                      audio_safe_generation);
}

int PublishedSampleStore::sample_length() const noexcept {
    const auto view = slots_.read_published_view();
    if (!view.valid) return 0;
    if (view.num_frames > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(view.num_frames);
}

bool PublishedSampleStore::populate_channel_ptrs(
    const PublishedSampleView& view,
    const float** channel_ptrs,
    std::size_t channel_capacity) const noexcept {
    if (channel_ptrs == nullptr ||
        !view.valid ||
        view.num_channels == 0 ||
        view.num_frames == 0 ||
        view.num_channels > channel_capacity ||
        view.num_channels > config_.max_channels) {
        return false;
    }
    for (std::uint32_t ch = 0; ch < view.num_channels; ++ch) {
        channel_ptrs[ch] = slots_.channel_data(view, ch);
        if (channel_ptrs[ch] == nullptr) return false;
    }
    return true;
}

}  // namespace pulp::audio
