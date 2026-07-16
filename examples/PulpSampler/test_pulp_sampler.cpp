#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_sampler.hpp"
#include "rt_allocation_probe.hpp"
#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

namespace pulp::examples {

struct PulpSamplerTestAccess {
    static void pause_stream_dispatch(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_.service_dispatch_paused_.store(
            paused, std::memory_order_release);
        processor.streaming_.service_wake_.notify_all();
    }

    static void set_reverse_prewarm_timeout(
        PulpSamplerProcessor& processor,
        std::chrono::milliseconds timeout) {
        processor.streaming_.reverse_prewarm_timeout_ = timeout;
    }

    static bool streamed_tail_page_ready(
        const PulpSamplerProcessor& processor) {
        const auto published = processor.streaming_.published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed ||
            !published.streamed.valid() || published.streamed.total_frames == 0) {
            return false;
        }
        const auto& stream = published.streamed.stream_source;
        return stream.window->ready_page_for_frame(
            published.streamed.source.source_generation,
            published.streamed.total_frames - 1).valid;
    }

    static SamplerPublishedSourceKind published_source_kind(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().kind;
    }

    static bool reverse_prewarm_pending(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.reverse_prewarm_pending_for_test_.load(
            std::memory_order_acquire);
    }

    static void block_next_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_.reverse_decode_entered_for_test_.store(
            false, std::memory_order_relaxed);
        processor.streaming_.release_reverse_decode_for_test_.store(
            false, std::memory_order_release);
        processor.streaming_.block_next_reverse_decode_for_test_.store(
            true, std::memory_order_release);
    }

    static bool reverse_decode_entered(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.reverse_decode_entered_for_test_.load(
            std::memory_order_acquire);
    }

    static void release_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_.release_reverse_decode_for_test_.store(
            true, std::memory_order_release);
    }

    static std::uint32_t unpublished_rollback_count(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.unpublished_rollback_count_for_test_.load(
            std::memory_order_acquire);
    }
};

}  // namespace pulp::examples

struct TempSamplerWav {
    std::string path;

    TempSamplerWav(const char* label, std::uint64_t frames, float value) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav")).string();
        audio::AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {std::vector<float>(static_cast<std::size_t>(frames), value)};
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    ~TempSamplerWav() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct SamplerProcessBlock {
    explicit SamplerProcessBlock(std::uint32_t frames = 512)
        : left(frames), right(frames), output_ptrs{left.data(), right.data()},
          output(output_ptrs, 2, frames), input(input_ptrs, 0, frames),
          context{44100, static_cast<int>(frames)} {}

    void run(PulpSamplerProcessor& processor) {
        processor.process(output, input, midi_in, midi_out, context);
    }

    std::vector<float> left;
    std::vector<float> right;
    float* output_ptrs[2];
    const float* input_ptrs[2]{nullptr, nullptr};
    audio::BufferView<float> output;
    audio::BufferView<const float> input;
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    format::ProcessContext context;
};

// Generate a 1-second sine wave at 440 Hz
static std::vector<float> make_sine(float freq = 440.0f, float sr = 44100.0f, int samples = 44100) {
    std::vector<float> data(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        data[static_cast<size_t>(i)] = std::sin(2.0f * 3.14159f * freq * static_cast<float>(i) / sr);
    }
    return data;
}

struct SamplerFixture {
    state::StateStore store;
    std::unique_ptr<PulpSamplerProcessor> proc;

    SamplerFixture() {
        proc = std::make_unique<PulpSamplerProcessor>();
        proc->set_state_store(&store);
        proc->define_parameters(store);

        format::PrepareContext ctx;
        ctx.sample_rate = 44100;
        ctx.max_buffer_size = 512;
        ctx.input_channels = 0;
        ctx.output_channels = 2;
        proc->prepare(ctx);

        auto sample = make_sine();
        REQUIRE(proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    }
};

template <typename Predicate>
static bool wait_for_condition(Predicate predicate,
                               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

struct LoaderThreadGuard {
    std::atomic<bool>& running;
    std::thread& loader;

    ~LoaderThreadGuard() {
        running.store(false, std::memory_order_release);
        if (loader.joinable()) {
            loader.join();
        }
    }
};

TEST_CASE("PulpSampler descriptor", "[sampler]") {
    PulpSamplerProcessor proc;
    auto d = proc.descriptor();
    REQUIRE(d.name == "PulpSampler");
    REQUIRE(d.category == format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.input_buses.empty());
    REQUIRE(d.output_buses.size() == 1);
}

TEST_CASE("PulpSampler has 7 parameters", "[sampler]") {
    SamplerFixture f;
    REQUIRE(f.store.param_count() == 7);
}

TEST_CASE("PulpSampler loads sample", "[sampler]") {
    SamplerFixture f;
    REQUIRE(f.proc->has_sample());
    REQUIRE(f.proc->sample_length() == 44100);
}

TEST_CASE("PulpSampler reports invalid sample loads", "[sampler]") {
    SamplerFixture f;
    REQUIRE_FALSE(f.proc->load_sample(nullptr, 128, 44100.0f));
    std::vector<float> sample(128, 0.0f);
    REQUIRE_FALSE(f.proc->load_sample(sample.data(), 0, 44100.0f));
    REQUIRE_FALSE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 0.0f));
    REQUIRE_FALSE(f.proc->load_sample_stereo(sample.data(),
                                             std::numeric_limits<int>::max(),
                                             44100.0f));
}

TEST_CASE("PulpSampler requires prepare before sample loading", "[sampler]") {
    state::StateStore store;
    PulpSamplerProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    std::vector<float> sample(128, 0.0f);
    REQUIRE_FALSE(proc.load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
}

TEST_CASE("PulpSampler silence without MIDI", "[sampler]") {
    SamplerFixture f;

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext ctx{44100, 512};

    f.proc->process(out, in, midi_in, midi_out, ctx);

    // No MIDI input → silence
    float sum = 0;
    for (int i = 0; i < 512; ++i) sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE_THAT(sum, WithinAbs(0.0, 0.001));
}

TEST_CASE("PulpSampler produces audio on note-on", "[sampler]") {
    SamplerFixture f;

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100)); // Middle C
    format::ProcessContext ctx{44100, 512};

    f.proc->process(out, in, midi_in, midi_out, ctx);

    // Should produce non-zero output
    float peak = 0;
    for (int i = 0; i < 512; ++i) {
        peak = std::max(peak, std::abs(out_l[static_cast<size_t>(i)]));
    }
    REQUIRE(peak > 0.01f);
}

TEST_CASE("PulpSampler process runs under no-allocation guard after prepare",
          "[sampler][rt]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
    format::ProcessContext ctx{44100, 512};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float peak = 0.0f;
    for (float sample : out_l) {
        peak = std::max(peak, std::abs(sample));
    }
    REQUIRE(peak > 0.01f);
}

TEST_CASE("PulpSampler handles dense MIDI and voice stealing under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(2048, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(256, 0), out_r(256, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 256);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 256);

    midi::MidiBuffer midi_in, midi_out;
    for (int i = 0; i <= PulpSamplerProcessor::kMaxVoices; ++i) {
        auto event = midi::MidiEvent::note_on(0, 60 + i, 127);
        event.sample_offset = static_cast<int32_t>(i * 8);
        midi_in.add(event);
    }
    format::ProcessContext ctx{44100, 256};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float sum = 0.0f;
    for (float sample_value : out_l) {
        sum += std::abs(sample_value);
    }
    REQUIRE(sum > 100.0f);
    REQUIRE(out_r[96] > 0.1f);
}

TEST_CASE("PulpSampler sorts and clamps MIDI offsets under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(1024, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(130, 123.0f), out_r(130, -123.0f);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);

    midi::MidiBuffer midi_in, midi_out;
    auto later_note = midi::MidiEvent::note_on(0, 60, 127);
    later_note.sample_offset = 64;
    midi_in.add(later_note);

    auto early_note = midi::MidiEvent::note_on(0, 62, 127);
    early_note.sample_offset = -32;
    midi_in.add(early_note);

    auto end_note_off = midi::MidiEvent::note_off(0, 62);
    end_note_off.sample_offset = 999;
    midi_in.add(end_note_off);

    format::ProcessContext ctx{44100, 128};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float early_sum = 0.0f;
    for (int i = 0; i < 16; ++i) early_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE(early_sum > 8.0f);
    REQUIRE(out_l[96] > 0.1f);
    REQUIRE(out_r[96] > 0.1f);
    REQUIRE_THAT(out_l[128], WithinAbs(123.0f, 0.0f));
    REQUIRE_THAT(out_r[129], WithinAbs(-123.0f, 0.0f));
}

TEST_CASE("PulpSampler tolerates controller-thread sample loads during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> sample_a(128, 0.35f);
    std::vector<float> sample_b(128, 0.9f);
    std::atomic<bool> loader_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> load_attempts{0};
    std::atomic<int> load_successes{0};

    std::thread loader([&] {
        loader_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? sample_a : sample_b;
            load_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample(source.data(),
                                    static_cast<int>(source.size()),
                                    44100.0f)) {
                load_successes.fetch_add(1, std::memory_order_relaxed);
            }
            if ((i % 8) == 0) {
                std::this_thread::yield();
            }
        }
    });
    LoaderThreadGuard loader_guard{running, loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return loader_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return load_successes.load(std::memory_order_relaxed) > 0;
    }));
    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);

    const int attempts_before_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_before_process = load_successes.load(std::memory_order_relaxed);
    float observed_peak = 0.0f;
    bool finite_output = true;
    for (int block = 0; block < 400; ++block) {
        midi_in.clear();
        if ((block % 8) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (float sample_value : out_l) {
            finite_output = finite_output && std::isfinite(sample_value);
            observed_peak = std::max(observed_peak, std::abs(sample_value));
        }
        for (float sample_value : out_r) {
            finite_output = finite_output && std::isfinite(sample_value);
        }
        std::this_thread::yield();
    }
    const int attempts_after_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_after_process = load_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (loader.joinable()) {
        loader.join();
    }

    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);
    REQUIRE(attempts_after_process > attempts_before_process);
    REQUIRE(successes_after_process > successes_before_process);
    REQUIRE(finite_output);
    REQUIRE(observed_peak > 0.1f);
}

TEST_CASE("PulpSampler tolerates controller-thread stereo loads during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    constexpr int kFrames = 128;
    std::vector<float> stereo_a(static_cast<std::size_t>(kFrames) * 2u);
    std::vector<float> stereo_b(static_cast<std::size_t>(kFrames) * 2u);
    for (int i = 0; i < kFrames; ++i) {
        stereo_a[static_cast<std::size_t>(i) * 2u] = 0.2f;
        stereo_a[static_cast<std::size_t>(i) * 2u + 1u] = 0.9f;
        stereo_b[static_cast<std::size_t>(i) * 2u] = 0.85f;
        stereo_b[static_cast<std::size_t>(i) * 2u + 1u] = 0.25f;
    }

    std::atomic<bool> loader_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> load_attempts{0};
    std::atomic<int> load_successes{0};

    std::thread loader([&] {
        loader_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? stereo_a : stereo_b;
            load_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample_stereo(source.data(), kFrames, 44100.0f)) {
                load_successes.fetch_add(1, std::memory_order_relaxed);
            }
            if ((i % 8) == 0) {
                std::this_thread::yield();
            }
        }
    });
    LoaderThreadGuard loader_guard{running, loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return loader_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return load_successes.load(std::memory_order_relaxed) > 0;
    }));
    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);

    const int attempts_before_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_before_process = load_successes.load(std::memory_order_relaxed);
    float peak_l = 0.0f;
    float peak_r = 0.0f;
    bool finite_output = true;
    bool channel_diverged = false;
    for (int block = 0; block < 400; ++block) {
        midi_in.clear();
        if ((block % 8) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (std::size_t i = 0; i < out_l.size(); ++i) {
            finite_output = finite_output &&
                            std::isfinite(out_l[i]) &&
                            std::isfinite(out_r[i]);
            peak_l = std::max(peak_l, std::abs(out_l[i]));
            peak_r = std::max(peak_r, std::abs(out_r[i]));
            channel_diverged = channel_diverged || std::abs(out_l[i] - out_r[i]) > 0.05f;
        }
        std::this_thread::yield();
    }
    const int attempts_after_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_after_process = load_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (loader.joinable()) {
        loader.join();
    }

    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);
    REQUIRE(attempts_after_process > attempts_before_process);
    REQUIRE(successes_after_process > successes_before_process);
    REQUIRE(finite_output);
    REQUIRE(peak_l > 0.05f);
    REQUIRE(peak_r > 0.05f);
    REQUIRE(channel_diverged);
}

TEST_CASE("PulpSampler serializes multiple controller loaders during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    constexpr int kFrames = 128;
    std::vector<float> mono_a(kFrames, 0.25f);
    std::vector<float> mono_b(kFrames, 0.8f);
    std::vector<float> stereo_a(static_cast<std::size_t>(kFrames) * 2u);
    std::vector<float> stereo_b(static_cast<std::size_t>(kFrames) * 2u);
    for (int i = 0; i < kFrames; ++i) {
        stereo_a[static_cast<std::size_t>(i) * 2u] = 0.15f;
        stereo_a[static_cast<std::size_t>(i) * 2u + 1u] = 0.95f;
        stereo_b[static_cast<std::size_t>(i) * 2u] = 0.9f;
        stereo_b[static_cast<std::size_t>(i) * 2u + 1u] = 0.2f;
    }

    std::atomic<bool> mono_ready{false};
    std::atomic<bool> stereo_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> mono_attempts{0};
    std::atomic<int> mono_successes{0};
    std::atomic<int> stereo_attempts{0};
    std::atomic<int> stereo_successes{0};

    std::thread mono_loader([&] {
        mono_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? mono_a : mono_b;
            mono_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample(source.data(),
                                    static_cast<int>(source.size()),
                                    44100.0f)) {
                mono_successes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            std::this_thread::yield();
        }
    });
    LoaderThreadGuard mono_guard{running, mono_loader};

    std::thread stereo_loader([&] {
        stereo_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? stereo_a : stereo_b;
            stereo_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample_stereo(source.data(), kFrames, 44100.0f)) {
                stereo_successes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            std::this_thread::yield();
        }
    });
    LoaderThreadGuard stereo_guard{running, stereo_loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return mono_ready.load(std::memory_order_acquire) &&
               stereo_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return mono_attempts.load(std::memory_order_relaxed) > 0 &&
               stereo_attempts.load(std::memory_order_relaxed) > 0;
    }));

    const int mono_attempts_before = mono_attempts.load(std::memory_order_relaxed);
    const int mono_successes_before = mono_successes.load(std::memory_order_relaxed);
    const int stereo_attempts_before = stereo_attempts.load(std::memory_order_relaxed);
    const int stereo_successes_before = stereo_successes.load(std::memory_order_relaxed);

    float peak_l = 0.0f;
    float peak_r = 0.0f;
    bool finite_output = true;
    for (int block = 0; block < 600; ++block) {
        midi_in.clear();
        if (block >= 150 && (block % 4) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (std::size_t i = 0; i < out_l.size(); ++i) {
            finite_output = finite_output &&
                            std::isfinite(out_l[i]) &&
                            std::isfinite(out_r[i]);
            peak_l = std::max(peak_l, std::abs(out_l[i]));
            peak_r = std::max(peak_r, std::abs(out_r[i]));
        }
        std::this_thread::yield();
    }

    const int mono_attempts_after = mono_attempts.load(std::memory_order_relaxed);
    const int mono_successes_after = mono_successes.load(std::memory_order_relaxed);
    const int stereo_attempts_after = stereo_attempts.load(std::memory_order_relaxed);
    const int stereo_successes_after = stereo_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (mono_loader.joinable()) {
        mono_loader.join();
    }
    if (stereo_loader.joinable()) {
        stereo_loader.join();
    }

    REQUIRE(mono_attempts_after > mono_attempts_before);
    REQUIRE(stereo_attempts_after > stereo_attempts_before);
    REQUIRE(mono_successes_after > mono_successes_before);
    REQUIRE(stereo_successes_after > stereo_successes_before);
    REQUIRE(finite_output);
    REQUIRE(peak_l > 0.05f);
    REQUIRE(peak_r > 0.05f);
}

TEST_CASE("PulpSampler handles note release under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(2048, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);
    f.store.set_value(kSamplerRelease, 50.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);
    format::ProcessContext ctx{44100, 128};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    auto note_off = midi::MidiEvent::note_off(0, 60);
    note_off.sample_offset = 64;
    midi_in.add(note_off);
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float held_sum = 0.0f;
    float release_sum = 0.0f;
    for (int i = 0; i < 64; ++i) held_sum += std::abs(out_l[static_cast<size_t>(i)]);
    for (int i = 64; i < 128; ++i) release_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE(held_sum > 40.0f);
    REQUIRE(release_sum > 40.0f);
}

TEST_CASE("PulpSampler loads interleaved stereo into separate channels",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> interleaved(512);
    for (std::size_t i = 0; i < 256; ++i) {
        interleaved[i * 2u] = 0.25f;
        interleaved[i * 2u + 1u] = 1.0f;
    }
    REQUIRE(f.proc->load_sample_stereo(interleaved.data(), 256, 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext ctx{44100, 128};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float peak_l = 0.0f;
    float peak_r = 0.0f;
    for (std::size_t i = 0; i < out_l.size(); ++i) {
        peak_l = std::max(peak_l, std::abs(out_l[i]));
        peak_r = std::max(peak_r, std::abs(out_r[i]));
    }
    REQUIRE(peak_l > 0.1f);
    REQUIRE(peak_r > peak_l * 2.0f);
}

TEST_CASE("PulpSampler respects MIDI sample offsets", "[sampler]") {
    SamplerFixture f;

    std::vector<float> sample(256, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    auto event = midi::MidiEvent::note_on(0, 60, 127);
    event.sample_offset = 128;
    midi_in.add(event);
    format::ProcessContext ctx{44100, 512};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float pre_sum = 0.0f;
    for (int i = 0; i < 128; ++i) pre_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE_THAT(pre_sum, WithinAbs(0.0, 0.001));
    REQUIRE(std::abs(out_l[200]) > 0.01f);
}

TEST_CASE("PulpSampler loops through the primitive loop renderer", "[sampler]") {
    SamplerFixture f;

    std::vector<float> sample(32, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerLoop, 1.0f);

    std::vector<float> out_l(256, 0), out_r(256, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 256);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 256);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext ctx{44100, 256};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(std::abs(out_l[64]) > 0.1f);
    REQUIRE(std::abs(out_l[128]) > 0.1f);
    REQUIRE(std::abs(out_r[128]) > 0.1f);
}

TEST_CASE("PulpSampler keeps active voices on their original sample generation",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> first(64, 1.0f);
    std::vector<float> second(64, 0.25f);
    REQUIRE(f.proc->load_sample(first.data(), static_cast<int>(first.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);
    f.store.set_value(kSamplerLoop, 1.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);
    format::ProcessContext ctx{44100, 128};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(f.proc->load_sample(second.data(), static_cast<int>(second.size()), 44100.0f));
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(out_l[16] > 0.75f);
    REQUIRE(out_r[16] > 0.75f);
}

TEST_CASE("PulpSampler clears per-voice scratch when short voices finish",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> first(64, 1.0f);
    REQUIRE(f.proc->load_sample(first.data(), static_cast<int>(first.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(16, 0), out_r(16, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 16);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 16);
    format::ProcessContext ctx{44100, 16};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    std::vector<float> second(4, 0.25f);
    REQUIRE(f.proc->load_sample(second.data(), static_cast<int>(second.size()), 44100.0f));
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(out_l[1] > 1.1f);
    REQUIRE(out_l[8] > 0.75f);
    REQUIRE(out_l[8] < 1.1f);
    REQUIRE(out_r[8] > 0.75f);
    REQUIRE(out_r[8] < 1.1f);
}

TEST_CASE("PulpSampler state round-trip", "[sampler]") {
    SamplerFixture f;

    f.store.set_value(kSamplerGain, -12.0f);
    f.store.set_value(kSamplerAttack, 50.0f);

    auto saved = f.store.serialize();
    REQUIRE_FALSE(saved.empty());

    f.store.reset_all_to_defaults();
    REQUIRE(f.store.deserialize(saved));
    REQUIRE(std::abs(f.store.get_value(kSamplerGain) - (-12.0f)) < 0.01f);
}

TEST_CASE("PulpSampler streams continuously across the preload boundary",
          "[sampler][stream]") {
    TempSamplerWav wav("boundary", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    const auto preload = fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < 24000);

    SamplerProcessBlock block;
    const auto pages_before_note =
        fixture.proc->stream_stats().pages_published;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().pages_published > pages_before_note;
    }));

    std::vector<float> rendered = block.left;
    block.midi_in.clear();
    while (rendered.size() < preload + 32) {
        block.run(*fixture.proc);
        rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    for (std::uint64_t frame = preload - 8; frame < preload + 8; ++frame) {
        REQUIRE_THAT(rendered[static_cast<std::size_t>(frame)], WithinAbs(0.5f, 1e-6));
    }
}

TEST_CASE("PulpSampler prewarms reverse entry before publishing a stream",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry", 24000, 0.5f);
    SamplerFixture fixture;

    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(fixture.proc->stream_stats().preload_frames < 24000);
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
}

TEST_CASE("PulpSampler does not publish a stream when reverse prewarm fails",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_failure", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(5));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    REQUIRE_FALSE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::Resident);
    REQUIRE(fixture.proc->stream_stats().active_sources == 0);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
}

TEST_CASE("PulpSampler keeps the resident source published while prewarm waits",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_ordering", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    std::atomic<bool> loaded{false};
    std::thread loader([&] {
        loaded.store(fixture.proc->load_sample_file(wav.path),
                     std::memory_order_release);
    });
    const bool reached_prewarm = wait_for_condition([&] {
        return PulpSamplerTestAccess::reverse_prewarm_pending(*fixture.proc);
    });
    const auto kind_while_pending =
        PulpSamplerTestAccess::published_source_kind(*fixture.proc);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    loader.join();

    REQUIRE(reached_prewarm);
    REQUIRE(kind_while_pending == SamplerPublishedSourceKind::Resident);
    REQUIRE(loaded.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
}

TEST_CASE("PulpSampler reclaims an in-flight failed prewarm registration",
          "[sampler][stream]") {
    TempSamplerWav failed("reverse_entry_in_flight", 24000, 0.5f);
    TempSamplerWav first("reverse_entry_reuse_a", 24000, 0.25f);
    TempSamplerWav second("reverse_entry_reuse_b", 24000, 0.75f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(20));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    const bool failed_load = fixture.proc->load_sample_file(failed.path);
    const bool decode_entered =
        PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc);
    const auto rollback_count =
        PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc);
    PulpSamplerTestAccess::release_reverse_decode(*fixture.proc);
    const bool rollback_completed = wait_for_condition([&] {
        return PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc) == 0;
    });
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(250));

    REQUIRE_FALSE(failed_load);
    REQUIRE(decode_entered);
    REQUIRE(rollback_count == 1);
    REQUIRE(rollback_completed);
    REQUIRE(fixture.proc->load_sample_file(first.path));
    REQUIRE(fixture.proc->load_sample_file(second.path));
}

TEST_CASE("PulpSampler shutdown rejects an in-flight prewarm admission",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_shutdown", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    std::atomic<bool> load_result{true};
    std::thread loader([&] {
        load_result.store(fixture.proc->load_sample_file(wav.path),
                          std::memory_order_release);
    });
    const bool decode_entered = wait_for_condition([&] {
        return PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc);
    });
    fixture.proc->release();
    loader.join();

    REQUIRE(decode_entered);
    REQUIRE_FALSE(load_result.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::None);
}

TEST_CASE("PulpSampler reports deterministic streamed starvation",
          "[sampler][stream]") {
    TempSamplerWav wav("starvation", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    const auto preload = fixture.proc->stream_stats().preload_frames;
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto blocks = static_cast<std::uint32_t>(preload / 512) + 3;
    for (std::uint32_t index = 1; index < blocks; ++index) {
        block.run(*fixture.proc);
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames > 0);
    REQUIRE_THAT(block.left.back(), WithinAbs(0.0f, 0.0f));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
}

TEST_CASE("PulpSampler keeps staggered streamed voices supplied",
          "[sampler][stream][polyphony]") {
    TempSamplerWav wav("polyphony", 200000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    for (int voice = 0; voice < PulpSamplerProcessor::kMaxVoices; ++voice) {
        block.midi_in.clear();
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        block.midi_in.clear();
        for (int spacing = 1; spacing < 16; ++spacing) {
            block.run(*fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (int tail = 0; tail < 20; ++tail) {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(fixture.proc->stream_stats().pages_published > 0);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE_THAT(block.left.front(), WithinAbs(2.0f, 1e-5));
}

TEST_CASE("PulpSampler retains a replaced stream until its voice acknowledges",
          "[sampler][stream]") {
    TempSamplerWav first("replacement_a", 24000, 0.75f);
    TempSamplerWav second("replacement_b", 24000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    REQUIRE(fixture.proc->stream_stats().active_sources == 2);
    REQUIRE(fixture.proc->stream_stats().sources_retired == 0);

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 60));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1 &&
               fixture.proc->stream_stats().sources_retired == 1;
    }));
}

TEST_CASE("PulpSampler streamed process stays allocation-free for 10000 callbacks",
          "[sampler][stream][rt]") {
    TempSamplerWav wav("rt", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    const auto note_on = midi::MidiEvent::note_on(0, 60, 127);
    block.midi_in.add(note_on);
    block.run(*fixture.proc);
    block.midi_in.clear();

    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        block.midi_in.clear();
        if ((callback % 512) == 0) block.midi_in.add(note_on);
        block.run(*fixture.proc);
    }
    REQUIRE(probe.allocation_count() == 0);
}
