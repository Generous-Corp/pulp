// audio_signal_generators.cpp — deterministic stimulus generators.
// Each generator's determinism contract is documented in the header; keep
// expressions here in lock-step with those docs.

#include "audio_signal_generators.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace pulp::test::audio {

namespace {

// SplitMix64 finalizer — scrambles (seed, channel) into a non-zero
// xorshift64* state. Documented in make_white_noise.
std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t channel) {
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (channel + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (z ^ (z >> 31)) | 1ULL; // never zero
}

// xorshift64* step; returns uniform double in [-1.0, 1.0).
//
// Uses the top 53 bits (the double mantissa width): `u >> 11` is an integer in
// [0, 2^53) that maps EXACTLY into a double, so `(u >> 11) / 2^53` is in [0, 1)
// with no rounding, and `* 2 - 1` is provably in [-1, 1). Dividing the full
// 64-bit `u` by 2^63 instead would double-round near the top of the range
// (values close to 2^64 round up to 2^64 exactly), yielding 1.0 and breaking
// the half-open contract.
double next_uniform(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const std::uint64_t u = state * 0x2545F4914F6CDD1DULL;
    const double unit = static_cast<double>(u >> 11) / 9007199254740992.0; // /2^53
    return unit * 2.0 - 1.0; // [-1.0, 1.0)
}

void require(bool ok, const char* what) {
    if (!ok)
        throw std::invalid_argument(what);
}

// Closed-form normalized phase in [0, 1) for sample `i` — the shared
// expression documented on make_saw/make_square. Computed from the index
// rather than accumulated, so a sample never depends on the block partition.
double wrapped_phase(double phase, double f0_hz, double sample_rate,
                     int i) {
    const double t = phase + f0_hz * i / sample_rate;
    return t - std::floor(t);
}

// Shared shape for the closed-form naive waveform generators: render channel
// 0 once, copy to the others (same signal per channel, per the header).
template <typename SampleFn>
pulp::audio::Buffer<float> generate_waveform(int channels, int frames,
                                             SampleFn&& sample_at) {
    auto buf = make_silence(channels, frames);
    if (frames == 0 || channels == 0)
        return buf;
    auto first = buf.channel(0);
    for (int i = 0; i < frames; ++i)
        first[static_cast<std::size_t>(i)] = sample_at(i);
    for (int ch = 1; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        std::copy(first.begin(), first.end(), span.begin());
    }
    return buf;
}

// Shared shape for the per-channel filtered-noise generators.
template <typename PerChannelFn>
pulp::audio::Buffer<float> generate_noise(int channels, int frames,
                                          std::uint64_t seed,
                                          PerChannelFn&& fill_channel) {
    require(channels >= 0 && frames >= 0, "noise: negative dimensions");
    pulp::audio::Buffer<float> buf(static_cast<std::size_t>(channels),
                                   static_cast<std::size_t>(frames));
    for (int ch = 0; ch < channels; ++ch) {
        auto state = mix_seed(seed, static_cast<std::uint64_t>(ch));
        fill_channel(buf.channel(static_cast<std::size_t>(ch)), state);
    }
    return buf;
}

} // namespace

pulp::audio::Buffer<float> make_silence(int channels, int frames) {
    require(channels >= 0 && frames >= 0, "make_silence: negative dimensions");
    return pulp::audio::Buffer<float>(static_cast<std::size_t>(channels),
                                      static_cast<std::size_t>(frames));
}

pulp::audio::Buffer<float> make_dc(int channels, int frames, float level) {
    auto buf = make_silence(channels, frames);
    for (int ch = 0; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        std::fill(span.begin(), span.end(), level);
    }
    return buf;
}

pulp::audio::Buffer<float> make_impulse(int channels, int frames,
                                        float amplitude, int position) {
    auto buf = make_silence(channels, frames);
    if (frames == 0)
        return buf;
    const auto pos = static_cast<std::size_t>(
        std::clamp(position, 0, frames - 1));
    for (int ch = 0; ch < channels; ++ch)
        buf.channel(static_cast<std::size_t>(ch))[pos] = amplitude;
    return buf;
}

pulp::audio::Buffer<float> make_impulse_train(int channels, int frames,
                                              int period_frames,
                                              float amplitude) {
    require(period_frames >= 1, "make_impulse_train: period must be >= 1");
    auto buf = make_silence(channels, frames);
    for (int ch = 0; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        for (int i = 0; i < frames; i += period_frames)
            span[static_cast<std::size_t>(i)] = amplitude;
    }
    return buf;
}

pulp::audio::Buffer<float> make_step(int channels, int frames, float level,
                                     int onset_frame) {
    auto buf = make_silence(channels, frames);
    for (int ch = 0; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        for (int i = std::max(onset_frame, 0); i < frames; ++i)
            span[static_cast<std::size_t>(i)] = level;
    }
    return buf;
}

pulp::audio::Buffer<float> make_multi_sine(int channels, int frames,
                                           std::span<const SinePartial> partials,
                                           double sample_rate) {
    require(sample_rate > 0.0, "make_multi_sine: sample_rate must be > 0");
    auto buf = make_silence(channels, frames);
    for (int ch = 0; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        for (int i = 0; i < frames; ++i) {
            double sum = 0.0;
            for (const auto& p : partials)
                sum += p.amplitude *
                       std::sin(2.0 * std::numbers::pi * p.hz * i / sample_rate);
            span[static_cast<std::size_t>(i)] = static_cast<float>(sum);
        }
    }
    return buf;
}

pulp::audio::Buffer<float> make_swept_sine(int channels, int frames,
                                           double start_hz, double end_hz,
                                           double sample_rate,
                                           float amplitude) {
    require(start_hz > 0.0 && end_hz > 0.0,
            "make_swept_sine: frequencies must be > 0");
    require(sample_rate > 0.0, "make_swept_sine: sample_rate must be > 0");
    auto buf = make_silence(channels, frames);
    if (frames == 0 || channels == 0)
        return buf;

    // Render channel 0 once, copy to the others (same signal per header).
    const double ratio = end_hz / start_hz;
    const double denom = frames > 1 ? static_cast<double>(frames - 1) : 1.0;
    auto first = buf.channel(0);
    double phase = 0.0;
    for (int i = 0; i < frames; ++i) {
        first[static_cast<std::size_t>(i)] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * phase));
        const double f = start_hz * std::pow(ratio, i / denom);
        phase += f / sample_rate;
    }
    for (int ch = 1; ch < channels; ++ch) {
        auto span = buf.channel(static_cast<std::size_t>(ch));
        std::copy(first.begin(), first.end(), span.begin());
    }
    return buf;
}

pulp::audio::Buffer<float> make_saw(int channels, int frames, double f0_hz,
                                    double sample_rate, float amplitude,
                                    double phase) {
    require(sample_rate > 0.0, "make_saw: sample_rate must be > 0");
    return generate_waveform(channels, frames,
        [=](int i) {
            const double p = wrapped_phase(phase, f0_hz, sample_rate, i);
            return static_cast<float>(amplitude * (2.0 * p - 1.0));
        });
}

pulp::audio::Buffer<float> make_square(int channels, int frames, double f0_hz,
                                       double sample_rate, float amplitude,
                                       double pulse_width, double phase) {
    require(sample_rate > 0.0, "make_square: sample_rate must be > 0");
    require(pulse_width >= 0.0 && pulse_width <= 1.0,
            "make_square: pulse_width must be in [0, 1]");
    return generate_waveform(channels, frames,
        [=](int i) {
            const double p = wrapped_phase(phase, f0_hz, sample_rate, i);
            return p < pulse_width ? amplitude : -amplitude;
        });
}

pulp::audio::Buffer<float> make_white_noise(int channels, int frames,
                                            std::uint64_t seed,
                                            float amplitude) {
    return generate_noise(channels, frames, seed,
        [amplitude](std::span<float> span, std::uint64_t& state) {
            for (auto& s : span)
                s = static_cast<float>(amplitude * next_uniform(state));
        });
}

pulp::audio::Buffer<float> make_pink_noise(int channels, int frames,
                                           std::uint64_t seed,
                                           float amplitude) {
    return generate_noise(channels, frames, seed,
        [amplitude](std::span<float> span, std::uint64_t& state) {
            // Kellett economy pinking filter; coefficients in the header.
            double b0 = 0.0, b1 = 0.0, b2 = 0.0;
            for (auto& s : span) {
                const double w = next_uniform(state);
                b0 = 0.99765 * b0 + w * 0.0990460;
                b1 = 0.96300 * b1 + w * 0.2965164;
                b2 = 0.57000 * b2 + w * 1.0526913;
                s = static_cast<float>(amplitude * 0.25 *
                                       (b0 + b1 + b2 + w * 0.1848));
            }
        });
}

pulp::audio::Buffer<float> make_brown_noise(int channels, int frames,
                                            std::uint64_t seed,
                                            float amplitude) {
    return generate_noise(channels, frames, seed,
        [amplitude](std::span<float> span, std::uint64_t& state) {
            double acc = 0.0;
            for (auto& s : span) {
                acc = 0.998 * acc + next_uniform(state) * 0.02;
                s = static_cast<float>(
                    std::clamp(amplitude * acc,
                               -static_cast<double>(amplitude),
                               static_cast<double>(amplitude)));
            }
        });
}

std::vector<ParamStep> make_stepped_automation(pulp::state::ParamID id,
                                               std::span<const float> values,
                                               std::int64_t start_frame,
                                               std::int64_t interval_frames) {
    require(interval_frames >= 1,
            "make_stepped_automation: interval must be >= 1");
    std::vector<ParamStep> steps;
    steps.reserve(values.size());
    for (std::size_t k = 0; k < values.size(); ++k)
        steps.push_back({id,
                         start_frame +
                             static_cast<std::int64_t>(k) * interval_frames,
                         values[k]});
    return steps;
}

std::vector<MidiScriptEvent> make_note_script(std::uint8_t note,
                                              std::uint8_t velocity,
                                              std::int64_t on_frame,
                                              std::int64_t off_frame,
                                              std::uint8_t channel) {
    std::vector<MidiScriptEvent> events;
    events.push_back(
        {on_frame, pulp::midi::MidiEvent::note_on(channel, note, velocity)});
    if (off_frame >= 0)
        events.push_back(
            {off_frame, pulp::midi::MidiEvent::note_off(channel, note)});
    return events;
}

} // namespace pulp::test::audio
