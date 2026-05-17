#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/format/test_signal.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define pulp_getpid() _getpid()
#else
#  include <unistd.h>
#  define pulp_getpid() ::getpid()
#endif

using namespace pulp::format;
using Catch::Matchers::WithinAbs;

namespace {

std::filesystem::path unique_temp_wav_path(std::string_view suffix) {
    // Combine pid + steady-clock nanoseconds + per-process counter so parallel
    // ctest invocations (`ctest -j4` in CI) don't collide on /tmp/pulp-test-signal-*.wav.
    // See pulp #1257 review comment r3175949820.
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto pid = static_cast<long long>(pulp_getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() /
           ("pulp-test-signal-" + std::to_string(pid) + "-" +
            std::to_string(now) + "-" + std::to_string(seq) + std::string(suffix));
}

pulp::audio::AudioFileData make_test_audio() {
    pulp::audio::AudioFileData data;
    data.sample_rate = 48000;
    data.channels = {
        {0.10f, 0.20f, 0.30f},
        {0.40f, 0.50f, 0.60f},
    };
    return data;
}

}  // namespace

TEST_CASE("TestSignalSource: inactive produces silence", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);

    constexpr int frames = 64;
    constexpr int channels = 2;
    std::vector<float> buf(channels * frames, 1.0f);
    float* ptrs[channels] = { buf.data(), buf.data() + frames };

    src.fill(ptrs, channels, frames);

    for (int i = 0; i < channels * frames; ++i) {
        REQUIRE(buf[static_cast<size_t>(i)] == 0.0f);
    }
}

TEST_CASE("TestSignalSource: sine tone produces non-zero output", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });

    constexpr int frames = 256;
    constexpr int channels = 2;
    std::vector<float> buf(channels * frames, 0.0f);
    float* ptrs[channels] = { buf.data(), buf.data() + frames };

    src.fill(ptrs, channels, frames);

    // Should have non-zero energy
    float energy = 0;
    for (int i = 0; i < frames; ++i) {
        energy += ptrs[0][i] * ptrs[0][i];
    }
    REQUIRE(energy > 0.1f);

    // Both channels should be identical (mono sine duplicated)
    for (int i = 0; i < frames; ++i) {
        REQUIRE_THAT(ptrs[0][i], WithinAbs(ptrs[1][i], 0.0001f));
    }

    // Peak should not exceed amplitude
    for (int i = 0; i < frames; ++i) {
        REQUIRE(std::abs(ptrs[0][i]) <= 0.51f);
    }
}

TEST_CASE("TestSignalSource: sine frequency is correct", "[format][test_signal]") {
    TestSignalSource src;
    double sr = 48000.0;
    src.set_sample_rate(sr);
    float freq = 1000.0f;
    src.set_config({ TestSignalType::sine, freq, 1.0f });

    // Generate one full period worth of samples
    int period_samples = static_cast<int>(sr / freq);
    std::vector<float> buf(static_cast<size_t>(period_samples), 0.0f);
    float* ptr = buf.data();

    src.fill(&ptr, 1, period_samples);

    // First sample should be near zero (sin(0))
    REQUIRE_THAT(buf[0], WithinAbs(0.0f, 0.01f));

    // Quarter-period sample should be near +1.0 (sin(π/2))
    int quarter = period_samples / 4;
    REQUIRE_THAT(buf[static_cast<size_t>(quarter)], WithinAbs(1.0f, 0.05f));
}

TEST_CASE("TestSignalSource: config change takes effect", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);

    // Start with sine
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });
    REQUIRE(src.is_active());

    // Switch to none
    src.set_config({ TestSignalType::none, 440.0f, 0.5f });

    constexpr int frames = 64;
    std::vector<float> buf(static_cast<size_t>(frames), 1.0f);
    float* ptr = buf.data();
    src.fill(&ptr, 1, frames);

    // Should be silence after switching to none
    float energy = 0;
    for (float s : buf) energy += s * s;
    REQUIRE(energy == 0.0f);
    REQUIRE(!src.is_active());
}

TEST_CASE("TestSignalSource: reset clears state", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });

    // Generate some samples to advance phase
    constexpr int frames = 256;
    std::vector<float> buf1(static_cast<size_t>(frames));
    std::vector<float> buf2(static_cast<size_t>(frames));
    float* ptr1 = buf1.data();
    float* ptr2 = buf2.data();

    src.fill(&ptr1, 1, frames);
    src.reset();

    // After reset + re-enable, output should start from phase 0 again
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });
    src.fill(&ptr2, 1, frames);

    // First sample should be near zero (phase reset)
    REQUIRE_THAT(buf2[0], WithinAbs(0.0f, 0.01f));
}

TEST_CASE("TestSignalSource: active flag follows config type",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    REQUIRE_FALSE(src.is_active());

    src.set_config({TestSignalType::sine, 440.0f, 0.5f});
    REQUIRE(src.is_active());

    src.set_config({TestSignalType::file, 440.0f, 0.5f});
    REQUIRE(src.is_active());

    src.set_config({TestSignalType::none, 440.0f, 0.5f});
    REQUIRE_FALSE(src.is_active());
}

TEST_CASE("TestSignalSource: config returns the last audio-thread-applied config",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    auto initial = src.config();
    REQUIRE(initial.type == TestSignalType::none);
    REQUIRE(initial.sine_frequency_hz == 440.0f);
    REQUIRE(initial.sine_amplitude == 0.5f);

    src.set_config({TestSignalType::sine, 220.0f, 0.25f});
    REQUIRE(src.config().type == TestSignalType::none);

    float sample = -1.0f;
    float* output = &sample;
    src.fill(&output, 1, 1);

    auto applied = src.config();
    REQUIRE(applied.type == TestSignalType::sine);
    REQUIRE(applied.sine_frequency_hz == 220.0f);
    REQUIRE(applied.sine_amplitude == 0.25f);
}

TEST_CASE("TestSignalSource: deterministic sine samples at quarter-cycle boundaries",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    src.set_sample_rate(4.0);
    src.set_config({TestSignalType::sine, 1.0f, 0.75f});

    std::vector<float> buf(4, 0.0f);
    float* output = buf.data();
    src.fill(&output, 1, 4);

    REQUIRE_THAT(buf[0], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(buf[1], WithinAbs(0.75f, 0.0001f));
    REQUIRE_THAT(buf[2], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(buf[3], WithinAbs(-0.75f, 0.0001f));
}

TEST_CASE("TestSignalSource: amplitude-only config changes keep phase continuity",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    src.set_sample_rate(4.0);
    src.set_config({TestSignalType::sine, 1.0f, 1.0f});

    float first = -1.0f;
    float* first_output = &first;
    src.fill(&first_output, 1, 1);
    REQUIRE_THAT(first, WithinAbs(0.0f, 0.0001f));

    src.set_config({TestSignalType::sine, 1.0f, 0.25f});
    float second = -1.0f;
    float* second_output = &second;
    src.fill(&second_output, 1, 1);

    REQUIRE_THAT(second, WithinAbs(0.25f, 0.0001f));
}

TEST_CASE("TestSignalSource: frequency changes reset sine phase",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    src.set_sample_rate(4.0);
    src.set_config({TestSignalType::sine, 1.0f, 1.0f});

    float first = -1.0f;
    float* first_output = &first;
    src.fill(&first_output, 1, 1);
    REQUIRE_THAT(first, WithinAbs(0.0f, 0.0001f));

    src.set_config({TestSignalType::sine, 2.0f, 1.0f});
    float after_change = -1.0f;
    float* changed_output = &after_change;
    src.fill(&changed_output, 1, 1);

    REQUIRE_THAT(after_change, WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("TestSignalSource: zero-amplitude sine stays active but silent",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({TestSignalType::sine, 440.0f, 0.0f});
    REQUIRE(src.is_active());

    std::vector<float> buf(64, 1.0f);
    float* output = buf.data();
    src.fill(&output, 1, static_cast<int>(buf.size()));

    for (float sample : buf) {
        REQUIRE(sample == 0.0f);
    }
    REQUIRE(src.is_active());
}

TEST_CASE("TestSignalSource: file mode without loaded data is silent and not playing",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    src.set_config({TestSignalType::file, 440.0f, 1.0f});
    REQUIRE(src.is_active());

    src.play();
    REQUIRE_FALSE(src.is_playing());
    REQUIRE_FALSE(src.has_file());

    std::vector<float> buf(8, -1.0f);
    float* output = buf.data();
    src.fill(&output, 1, static_cast<int>(buf.size()));

    for (float sample : buf) {
        REQUIRE(sample == 0.0f);
    }
    REQUIRE(src.file_position() == 0);
}

TEST_CASE("TestSignalSource: loop flag toggles independently of loaded file state",
          "[format][test_signal][coverage][phase3]") {
    TestSignalSource src;
    REQUIRE_FALSE(src.is_looping());

    src.set_loop(true);
    REQUIRE(src.is_looping());

    src.set_loop(false);
    REQUIRE_FALSE(src.is_looping());
}

TEST_CASE("TestSignalSource: invalid file load and unload reset playback state",
          "[format][test_signal][file]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);

    auto missing = unique_temp_wav_path("-missing.wav");
    std::filesystem::remove(missing);

    REQUIRE_FALSE(src.load_file(missing.string()));
    REQUIRE_FALSE(src.has_file());
    REQUIRE_FALSE(src.is_playing());
    REQUIRE(src.file_position() == 0);

    auto path = unique_temp_wav_path(".wav");
    std::filesystem::remove(path);
    REQUIRE(pulp::audio::write_wav_file(path.string(), make_test_audio()));

    REQUIRE(src.load_file(path.string()));
    REQUIRE(src.has_file());
    REQUIRE_FALSE(src.is_playing());

    src.play();
    REQUIRE(src.is_playing());

    src.stop();
    REQUIRE_FALSE(src.is_playing());
    REQUIRE(src.file_position() == 0);

    src.play();
    REQUIRE(src.is_playing());
    src.unload_file();
    REQUIRE_FALSE(src.has_file());
    REQUIRE_FALSE(src.is_playing());
    REQUIRE(src.file_position() == 0);

    std::filesystem::remove(path);
}

TEST_CASE("TestSignalSource: loading a new file rewinds and stops existing playback",
          "[format][test_signal][file][coverage][phase3]") {
    TestSignalSource src;
    src.set_config({TestSignalType::file, 440.0f, 1.0f});

    auto first_path = unique_temp_wav_path("-first.wav");
    auto second_path = unique_temp_wav_path("-second.wav");
    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    REQUIRE(pulp::audio::write_wav_file(first_path.string(), make_test_audio()));
    REQUIRE(pulp::audio::write_wav_file(second_path.string(), make_test_audio()));

    REQUIRE(src.load_file(first_path.string()));
    src.play();
    REQUIRE(src.is_playing());

    float buf[2] = {-1.0f, -1.0f};
    float* output = buf;
    src.fill(&output, 1, 2);
    REQUIRE(src.file_position() == 2);

    REQUIRE(src.load_file(second_path.string()));
    REQUIRE(src.has_file());
    REQUIRE_FALSE(src.is_playing());
    REQUIRE(src.file_position() == 0);
    REQUIRE(src.file_data() != nullptr);
    REQUIRE(src.file_data()->num_frames() == 3);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
}

TEST_CASE("TestSignalSource: reset stops file playback and rewinds without unloading",
          "[format][test_signal][file][coverage][phase3]") {
    TestSignalSource src;
    src.set_config({TestSignalType::file, 440.0f, 1.0f});

    auto path = unique_temp_wav_path(".wav");
    std::filesystem::remove(path);
    REQUIRE(pulp::audio::write_wav_file(path.string(), make_test_audio()));
    REQUIRE(src.load_file(path.string()));

    src.play();
    float played[2] = {-1.0f, -1.0f};
    float* played_output = played;
    src.fill(&played_output, 1, 2);
    REQUIRE(src.file_position() == 2);
    REQUIRE(src.is_playing());

    src.reset();
    REQUIRE(src.has_file());
    REQUIRE_FALSE(src.is_playing());
    REQUIRE(src.file_position() == 0);

    float silent[2] = {-1.0f, -1.0f};
    float* silent_output = silent;
    src.fill(&silent_output, 1, 2);
    REQUIRE(silent[0] == 0.0f);
    REQUIRE(silent[1] == 0.0f);
    REQUIRE(src.file_position() == 0);

    std::filesystem::remove(path);
}

TEST_CASE("TestSignalSource: file playback covers silence, EOF, loop, and channel fallback",
          "[format][test_signal][file]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({TestSignalType::file, 440.0f, 1.0f});

    auto path = unique_temp_wav_path(".wav");
    std::filesystem::remove(path);
    REQUIRE(pulp::audio::write_wav_file(path.string(), make_test_audio()));
    REQUIRE(src.load_file(path.string()));

    constexpr int frames = 5;
    std::vector<float> left(static_cast<size_t>(frames), -1.0f);
    std::vector<float> right(static_cast<size_t>(frames), -1.0f);
    std::vector<float> extra(static_cast<size_t>(frames), -1.0f);
    float* outputs[3] = {left.data(), right.data(), extra.data()};

    src.fill(outputs, 3, frames);
    for (int i = 0; i < frames; ++i) {
        REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
        REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
        REQUIRE(extra[static_cast<size_t>(i)] == 0.0f);
    }
    REQUIRE(src.file_position() == 0);
    REQUIRE_FALSE(src.is_playing());

    src.play();
    src.fill(outputs, 3, 2);
    REQUIRE_THAT(left[0], WithinAbs(0.10f, 0.001f));
    REQUIRE_THAT(left[1], WithinAbs(0.20f, 0.001f));
    REQUIRE_THAT(right[0], WithinAbs(0.40f, 0.001f));
    REQUIRE_THAT(right[1], WithinAbs(0.50f, 0.001f));
    REQUIRE_THAT(extra[0], WithinAbs(left[0], 0.001f));
    REQUIRE_THAT(extra[1], WithinAbs(left[1], 0.001f));
    REQUIRE(src.file_position() == 2);
    REQUIRE(src.is_playing());

    std::fill(left.begin(), left.end(), -1.0f);
    std::fill(right.begin(), right.end(), -1.0f);
    std::fill(extra.begin(), extra.end(), -1.0f);
    src.fill(outputs, 3, frames);
    REQUIRE_THAT(left[0], WithinAbs(0.30f, 0.001f));
    REQUIRE_THAT(right[0], WithinAbs(0.60f, 0.001f));
    REQUIRE_THAT(extra[0], WithinAbs(left[0], 0.001f));
    for (int i = 1; i < frames; ++i) {
        REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
        REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
        REQUIRE(extra[static_cast<size_t>(i)] == 0.0f);
    }
    REQUIRE(src.file_position() == 3);
    REQUIRE_FALSE(src.is_playing());

    src.set_loop(true);
    src.play();
    std::fill(left.begin(), left.end(), -1.0f);
    src.fill(outputs, 1, frames);
    REQUIRE_THAT(left[0], WithinAbs(0.10f, 0.001f));
    REQUIRE_THAT(left[1], WithinAbs(0.20f, 0.001f));
    REQUIRE_THAT(left[2], WithinAbs(0.30f, 0.001f));
    REQUIRE_THAT(left[3], WithinAbs(0.10f, 0.001f));
    REQUIRE_THAT(left[4], WithinAbs(0.20f, 0.001f));
    REQUIRE(src.file_position() == 2);
    REQUIRE(src.is_playing());

    std::filesystem::remove(path);
}
