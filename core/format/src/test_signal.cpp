#include <pulp/format/test_signal.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <cstring>

namespace pulp::format {

void TestSignalSource::set_config(const TestSignalConfig& cfg) {
    config_buf_.write(cfg);
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
    // Read latest config from UI thread via lock-free TripleBuffer
    auto& latest = config_buf_.read();
    if (latest.type != config_.type || latest.sine_frequency_hz != config_.sine_frequency_hz) {
        config_ = latest;
        if (config_.type == TestSignalType::sine) {
            sine_phase_ = 0.0;  // Reset phase on config change
        }
    } else {
        config_ = latest;
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

    if (config_.type == TestSignalType::noise) {
        // White noise from a per-channel LCG — a flat broadband source, so a
        // spectrum analyzer shows the EQ curve carved into the noise floor.
        const float amp = config_.sine_amplitude;  // reuse the amplitude field
        for (int ch = 0; ch < num_channels; ++ch) {
            std::uint32_t s = noise_state_[static_cast<size_t>(ch) % noise_state_.size()];
            for (int i = 0; i < num_samples; ++i) {
                s = s * 1664525u + 1013904223u;
                // Top 24 bits → [-1, 1).
                const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
                output[ch][i] = amp * white;
            }
            noise_state_[static_cast<size_t>(ch) % noise_state_.size()] = s;
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
