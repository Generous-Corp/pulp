#include "instrument.hpp"
#include "tuning_control.hpp"

#include <pulp/audio/analysis/pitch_track.hpp>
#include <pulp/audio/streaming_writer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/midi/mts_esp_tuning.hpp>
#include <pulp/midi/scala_tuning.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t kFirst = 8191;
constexpr std::size_t kSecond = 24577;
constexpr std::size_t kFrames = 40960;
constexpr int kMaximumBlock = 256;
constexpr double kAudioTolerance = 1e-6;
constexpr double kPhaseTolerance = 1e-9;

void require(bool condition, std::string_view reason) {
    if (!condition)
        throw std::runtime_error(std::string(reason));
}

double frequency_at(std::size_t frame) {
    return frame < kFirst ? 440.0 : frame < kSecond ? 660.0 : 442.0;
}

// Integrate the declared timeline in one expression, independently of the
// oscillator's per-sample accumulator and of the callback partitioning.
double phase_at(std::size_t frame, double rate, bool sibling) {
    const double cycles = sibling ? static_cast<double>(frame) * 330.0 / rate
        : (static_cast<double>(std::min(frame, kFirst)) * 440.0 +
           static_cast<double>(frame > kFirst ? std::min(frame, kSecond) - kFirst : 0) * 660.0 +
           static_cast<double>(frame > kSecond ? frame - kSecond : 0) * 442.0) / rate;
    const double phase = (sibling ? 0.271 : 0.137) + cycles;
    return phase - std::floor(phase);
}

double phase_error(double actual, double expected) {
    const double difference = std::abs(actual - expected);
    return std::min(difference, 1.0 - difference);
}

// This test double supplies malformed boundary input; it does not implement
// tuning mathematics. Real accepted changes use the public providers below.
class QueryFixture final : public pulp::midi::TuningProvider {
  public:
    pulp::midi::TuningQueryResult query;
    pulp::midi::TuningQueryResult note_to_frequency(int, int) const override { return query; }
};

struct Render {
    struct Change {
        std::size_t sample = 0;
        float old_hz = 0.0f;
        float new_hz = 0.0f;
        double phase_before = 0.0;
        double phase_after = 0.0;
    };
    std::array<std::vector<float>, 2> audio;
    std::array<Change, 2> changes{};
    double max_audio_error = 0.0;
    double max_phase_error = 0.0;
    std::size_t refusals = 0;
};

void refusal_controls(pulp::format::ProcessorNodeInstance& instance, g11::Instrument& voice,
                      double rate, std::size_t& count) {
    const float old_frequency = instance.state_store().get_value(g11::kFrequency);
    const double old_phase = voice.phase();
    const double old_envelope = voice.envelope();
    const auto old_frames = voice.frames();
    pulp::midi::EqualTemperamentTuningProvider valid(660.0);
    const auto refuse = [&](const pulp::midi::TuningProvider* provider, int note,
                            std::string_view path, g11::RetuneResult expected) {
        require(g11::retune(instance, provider, note, path, rate) == expected,
                "refusal code mismatch");
        require(instance.state_store().get_value(g11::kFrequency) == old_frequency &&
                    voice.phase() == old_phase && voice.envelope() == old_envelope &&
                    voice.frames() == old_frames && voice.prepares() == 1 && voice.note_ons() == 1,
                "refusal changed old state");
        ++count;
    };
    refuse(nullptr, 69, g11::kFrequencyPath, g11::RetuneResult::Unsupported);
    refuse(&valid, -1, g11::kFrequencyPath, g11::RetuneResult::InvalidNote);
    refuse(&valid, 128, g11::kFrequencyPath, g11::RetuneResult::InvalidNote);
    refuse(&valid, 69, "voice.unsupported", g11::RetuneResult::UnknownParameter);
    QueryFixture malformed;
    malformed.query = {.valid = false, .frequency_hz = 660.0};
    refuse(&malformed, 69, g11::kFrequencyPath, g11::RetuneResult::InvalidNote);
    malformed.query = {.valid = true, .frequency_hz = 660.0, .should_filter_note = true};
    refuse(&malformed, 69, g11::kFrequencyPath, g11::RetuneResult::InvalidNote);
    for (double invalid : {0.0, -1.0, 19.0, 2001.0, rate / 2.0,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        malformed.query = {.valid = true, .frequency_hz = invalid};
        refuse(&malformed, 69, g11::kFrequencyPath, g11::RetuneResult::InvalidFrequency);
    }
}

Render render(double rate, std::span<const int> partition, std::string_view planted,
              const pulp::midi::TuningProvider& first_provider,
              const pulp::midi::TuningProvider& second_provider) {
    pulp::host::SignalGraph graph;
    auto owned_voice = std::make_unique<g11::Instrument>(440.0, 0.137);
    auto owned_sibling = std::make_unique<g11::Instrument>(330.0, 0.271);
    auto* voice = owned_voice.get();
    auto* sibling = owned_sibling.get();
    const auto target = graph.add_processor_node(std::move(owned_voice), "Retuned voice");
    const auto other = graph.add_processor_node(std::move(owned_sibling), "Unchanged sibling");
    const auto midi = graph.add_midi_input_node("Keyboard");
    const auto output = graph.add_output_node(2, "Independent voices");
    require(target && other && midi && output && graph.connect(target, 0, output, 0) &&
                graph.connect(other, 0, output, 1) && graph.connect_midi(midi, target) &&
                graph.connect_midi(midi, other), "graph construction failed");
    auto instance = graph.processor_node_instance(target);
    auto sibling_instance = graph.processor_node_instance(other);
    require(instance && sibling_instance && graph.prepare(rate, kMaximumBlock), "prepare failed");
    pulp::midi::MidiBuffer notes;
    notes.reserve(1);
    require(notes.add(pulp::midi::MidiEvent::note_on(0, 69, 100)) && graph.inject_midi(midi, notes),
            "note injection failed");

    Render result;
    for (auto& channel : result.audio)
        channel.resize(kFrames);
    std::array<std::array<float, kMaximumBlock>, 2> block{};
    float* channels[] = {block[0].data(), block[1].data()};
    std::array<float, kMaximumBlock> silence{};
    const float* input_channels[] = {silence.data()};
    const auto audio_thread = std::this_thread::get_id();
    double envelope = 0.0;
    enum class Stage { Attack, Decay, Sustain } stage = Stage::Attack;
    std::size_t frame = 0;
    std::size_t part = 0;
    const std::size_t first_boundary = kFirst + (planted == "late" ? 1 : 0);
    while (frame < kFrames) {
        if (frame == first_boundary || frame == kSecond) {
            const double phase_before = voice->phase();
            const double envelope_before = voice->envelope();
            const float old_hz = instance->state_store().get_value(g11::kFrequency);
            g11::RetuneResult applied = g11::RetuneResult::Unsupported;
            bool external_thread = false;
            std::exception_ptr control_error;
            // Joining is an offline host synchronization point, never inside
            // process(). It pins the control write before this callback starts.
            std::thread control([&] {
                try {
                    external_thread = std::this_thread::get_id() != audio_thread;
                    refusal_controls(*instance, *voice, rate, result.refusals);
                    const auto& provider = frame == first_boundary ? first_provider : second_provider;
                    applied = g11::retune(*instance, &provider, 69, g11::kFrequencyPath, rate);
                } catch (...) {
                    control_error = std::current_exception();
                }
            });
            control.join();
            if (control_error)
                std::rethrow_exception(control_error);
            require(external_thread && applied == g11::RetuneResult::Applied,
                    "external control write failed");
            require(voice->phase() == phase_before && voice->envelope() == envelope_before,
                    "control write reset state");
            result.changes[frame == first_boundary ? 0 : 1] = {
                frame, old_hz, instance->state_store().get_value(g11::kFrequency),
                phase_before, voice->phase()};
            if (planted == "reset")
                voice->prepare({.sample_rate = rate});
            if (planted == "sibling")
                sibling_instance->state_store().set_value(g11::kFrequency, 660.0f);
        }
        std::size_t next_boundary = frame < first_boundary ? first_boundary
                                     : frame < kSecond ? kSecond : kFrames;
        const auto count = std::min({static_cast<std::size_t>(partition[part++ % partition.size()]),
                                     next_boundary - frame, kFrames - frame});
        require(count > 0 && count <= kMaximumBlock, "illegal partition");
        pulp::audio::BufferView<float> out(channels, 2, count);
        const pulp::audio::BufferView<const float> input(input_channels, 1, count);
        graph.process(out, input, static_cast<int>(count));
        for (std::size_t local = 0; local < count; ++local) {
            if (stage == Stage::Attack) {
                envelope += 1.0 / (0.01 * rate);
                if (envelope >= 1.0) { envelope = 1.0; stage = Stage::Decay; }
            } else if (stage == Stage::Decay) {
                envelope -= 1.0 / (0.1 * rate);
                if (envelope <= 0.7) { envelope = 0.7; stage = Stage::Sustain; }
            }
            for (int channel = 0; channel < 2; ++channel) {
                const auto position = frame + local;
                const double expected = 0.5 * envelope * std::sin(
                    2.0 * std::numbers::pi * phase_at(position, rate, channel == 1));
                const float actual = block[channel][local];
                require(std::isfinite(actual), "nonfinite rendered sample");
                result.max_audio_error = std::max(result.max_audio_error,
                    std::abs(static_cast<double>(actual) - expected));
                if (std::abs(static_cast<double>(actual) - expected) > kAudioTolerance)
                    std::cerr << "sample=" << position << " channel=" << channel
                              << " actual=" << actual << " expected=" << expected
                              << " envelope=" << envelope << '\n';
                require(std::abs(static_cast<double>(actual) - expected) <= kAudioTolerance,
                        channel == 0 ? "target audio oracle mismatch" : "sibling audio oracle mismatch");
                result.audio[channel][position] = actual;
            }
        }
        frame += count;
        for (int channel = 0; channel < 2; ++channel) {
            const auto* instrument = channel == 0 ? voice : sibling;
            const double error = phase_error(instrument->phase(), phase_at(frame, rate, channel == 1));
            result.max_phase_error = std::max(result.max_phase_error, error);
            require(error <= kPhaseTolerance, channel == 0 ? "target phase oracle mismatch"
                                                          : "sibling phase oracle mismatch");
            require(instrument->frames() == frame && instrument->prepares() == 1 &&
                        instrument->note_ons() == 1 && std::abs(instrument->envelope() - envelope) < 1e-10,
                    "instrument lifecycle/envelope changed");
        }
    }
    require(result.refusals == 28, "missing refusal controls");
    return result;
}

void inspect_pitch(const Render& rendered, double rate) {
    for (const auto start : {std::size_t{4096}, std::size_t{12288}, std::size_t{28672}}) {
        const auto estimate = pulp::test::audio::estimate_pitch(
            std::span<const float>(rendered.audio[0]).subspan(start, 4096), rate);
        require(estimate.voiced && estimate.confidence > 0.99 &&
                    std::abs(estimate.hz - frequency_at(start)) < 0.01, "rendered pitch oracle mismatch");
        std::cout << "pitch sample=" << start << " hz=" << estimate.hz
                  << " confidence=" << estimate.confidence << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string_view planted;
        std::filesystem::path artifacts;
        if (argc == 2 && std::string_view(argv[1]).starts_with("--planted-")) {
            planted = std::string_view(argv[1]).substr(10);
            require(planted == "late" || planted == "reset" || planted == "sibling", "unknown control");
        } else if (argc == 3 && std::string_view(argv[1]) == "--artifacts") {
            artifacts = argv[2];
        } else {
            require(argc == 1, "usage: consumer [--artifacts directory | --planted-late/reset/sibling]");
        }
        std::cout << std::setprecision(17);
        pulp::midi::EqualTemperamentTuningProvider first(660.0), second(442.0);
        const std::array<std::vector<int>, 6> partitions = {{{1}, {32}, {64}, {128}, {256}, {17, 127, 3, 251}}};
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            Render baseline;
            for (std::size_t index = 0; index < partitions.size(); ++index) {
                auto result = render(rate, partitions[index], planted, first, second);
                if (index == 0) {
                    baseline = result;
                    for (const auto& change : result.changes)
                        std::cout << "retune sample=" << change.sample << " old_hz=" << change.old_hz
                                  << " new_hz=" << change.new_hz << " phase_before=" << change.phase_before
                                  << " phase_after=" << change.phase_after
                                  << " phase_oracle=" << phase_at(change.sample, rate, false) << '\n';
                    inspect_pitch(result, rate);
                } else {
                    require(result.audio == baseline.audio, "partition bit parity mismatch");
                }
                std::cout << "render rate=" << rate << " partition=" << index
                          << " max_audio_error=" << result.max_audio_error
                          << " max_phase_error=" << result.max_phase_error
                          << " refusals=" << result.refusals << '\n';
            }
            if (rate == 48000.0 && !artifacts.empty()) {
                std::filesystem::create_directories(artifacts);
                pulp::audio::StreamingWriter writer;
                require(writer.open((artifacts / "live-retune.wav").string(), 48000, 2, 24), "WAV open failed");
                const float* channels[] = {baseline.audio[0].data(), baseline.audio[1].data()};
                require(writer.write_frames(channels, 2, static_cast<int>(kFrames)) == kFrames, "WAV write failed");
                writer.close();
            }
        }
#if PULP_HAS_SCALA_TUNING
        pulp::midi::ScalaTuningProvider scala;
        std::string error;
        require(scala.load_scl_data("! fifth\nFifth\n1\n3/2\n", &error) &&
                    scala.tune_note_to(69, 660.0, &error), "Scala preparation failed");
        const auto scala_render = render(48000, partitions.back(), {}, scala, second);
        inspect_pitch(scala_render, 48000);
        std::cout << "Scala live boundary render PASS\n";
#else
        std::cout << "Scala NOT CHECKED: SDK feature disabled; unsupported selection refused\n";
#endif
        std::cout << "MTS external master NOT CHECKED; provider interface reused, no session master fixture\n";
        require(planted.empty(), "planted defect escaped the oracle");
        std::cout << "G11 live-retune PASS boundaries=8191,24577 frequencies=440,660,442 frames=40960 renders=18\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "G11 refusal: " << error.what() << '\n';
        return 1;
    }
}
