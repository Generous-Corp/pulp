#include <pulp/format/test_signal.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <cstring>

namespace pulp::format {

void TestSignalSource::set_config(const TestSignalConfig& cfg) {
    std::lock_guard lock(config_mutex_);
    pending_config_ = cfg;
    config_dirty_.store(true, std::memory_order_release);
    active_.store(cfg.type != TestSignalType::none, std::memory_order_relaxed);
}

TestSignalConfig TestSignalSource::config() const {
    // Return the last-applied config (safe from UI thread for display)
    return config_;
}

bool TestSignalSource::load_file(const std::string& path) {
    auto result = audio::read_audio_file(path);
    if (!result) {
        runtime::log_warn("TestSignal: could not load '{}'", path);
        return false;
    }
    file_data_ = std::make_unique<audio::AudioFileData>(std::move(*result));
    file_position_.store(0, std::memory_order_relaxed);
    file_playing_.store(false, std::memory_order_relaxed);
    runtime::log_info("TestSignal: loaded '{}' ({} channels, {} frames, {} Hz)",
                      path, file_data_->num_channels(), file_data_->num_frames(),
                      file_data_->sample_rate);
    return true;
}

void TestSignalSource::unload_file() {
    file_playing_.store(false, std::memory_order_relaxed);
    file_data_.reset();
    file_position_.store(0, std::memory_order_relaxed);
}

void TestSignalSource::play() {
    if (file_data_) {
        file_playing_.store(true, std::memory_order_relaxed);
    }
}

void TestSignalSource::stop() {
    file_playing_.store(false, std::memory_order_relaxed);
    file_position_.store(0, std::memory_order_relaxed);
}

void TestSignalSource::reset() {
    sine_phase_ = 0.0;
    file_position_.store(0, std::memory_order_relaxed);
    file_playing_.store(false, std::memory_order_relaxed);
}

void TestSignalSource::fill(float* const* output, int num_channels, int num_samples) {
    // Check for pending config update from UI thread
    if (config_dirty_.load(std::memory_order_acquire)) {
        config_ = pending_config_;
        config_dirty_.store(false, std::memory_order_release);
        if (config_.type == TestSignalType::sine) {
            sine_phase_ = 0.0;  // Reset phase on config change
        }
    }

    if (config_.type == TestSignalType::none) {
        // Zero-fill
        for (int ch = 0; ch < num_channels; ++ch) {
            std::memset(output[ch], 0, static_cast<size_t>(num_samples) * sizeof(float));
        }
        return;
    }

    if (config_.type == TestSignalType::sine) {
        double phase_inc = 2.0 * 3.14159265358979323846 * config_.sine_frequency_hz / sample_rate_;
        float amp = config_.sine_amplitude;

        for (int i = 0; i < num_samples; ++i) {
            float sample = amp * static_cast<float>(std::sin(sine_phase_));
            for (int ch = 0; ch < num_channels; ++ch) {
                output[ch][i] = sample;
            }
            sine_phase_ += phase_inc;
        }
        // Wrap phase to avoid precision loss over long runs
        if (sine_phase_ > 2.0 * 3.14159265358979323846 * 1000.0) {
            sine_phase_ = std::fmod(sine_phase_, 2.0 * 3.14159265358979323846);
        }
        return;
    }

    if (config_.type == TestSignalType::file && file_data_ && file_playing_.load(std::memory_order_relaxed)) {
        int64_t pos = file_position_.load(std::memory_order_relaxed);
        int64_t total = static_cast<int64_t>(file_data_->num_frames());
        int file_ch = static_cast<int>(file_data_->num_channels());

        for (int i = 0; i < num_samples; ++i) {
            if (pos >= total) {
                if (file_loop_.load(std::memory_order_relaxed)) {
                    pos = 0;
                } else {
                    // Fill remainder with silence
                    for (int ch = 0; ch < num_channels; ++ch) {
                        std::memset(output[ch] + i, 0,
                                    static_cast<size_t>(num_samples - i) * sizeof(float));
                    }
                    file_playing_.store(false, std::memory_order_relaxed);
                    break;
                }
            }

            for (int ch = 0; ch < num_channels; ++ch) {
                int src_ch = (ch < file_ch) ? ch : 0;  // Mono→stereo: duplicate ch0
                output[ch][i] = file_data_->channels[static_cast<size_t>(src_ch)]
                                                     [static_cast<size_t>(pos)];
            }
            ++pos;
        }
        file_position_.store(pos, std::memory_order_relaxed);
        return;
    }

    // Fallback: silence
    for (int ch = 0; ch < num_channels; ++ch) {
        std::memset(output[ch], 0, static_cast<size_t>(num_samples) * sizeof(float));
    }
}

} // namespace pulp::format
