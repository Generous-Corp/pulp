#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/buffering_reader.hpp>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>
#include <vector>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

bool wait_for_frames(BufferingReader& reader, int min_frames) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reader.frames_available() < min_frames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return reader.frames_available() >= min_frames;
}

bool wait_until_finished_with_frames(BufferingReader& reader, int min_frames) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!reader.is_finished() || reader.frames_available() < min_frames) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return reader.is_finished() && reader.frames_available() >= min_frames;
}

} // namespace

TEST_CASE("BufferingReader reads from callback", "[audio][buffering]") {
    BufferingReader reader;

    int frame_counter = 0;
    reader.set_read_callback([&](float* dest, int frames, int channels) {
        for (int i = 0; i < frames * channels; ++i) {
            dest[i] = static_cast<float>(frame_counter++);
        }
        return frames;
    });

    reader.start(1, 4096);

    // Wait for background thread to fill some buffer (up to 2s)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reader.frames_available() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(reader.frames_available() > 0);

    float buf[256];
    int got = reader.read(buf, 256, 1);
    REQUIRE(got > 0);

    // First sample should be 0.0f (sequential counter)
    REQUIRE_THAT(buf[0], WithinAbs(0.0, 0.001));

    reader.stop();
}

TEST_CASE("BufferingReader partial read zero-fills after source ends",
          "[audio][buffering][issue-640]") {
    BufferingReader reader;

    int emitted = 0;
    reader.set_read_callback([&](float* dest, int frames, int channels) {
        int frames_to_emit = std::min(frames, 3 - emitted);
        for (int frame = 0; frame < frames_to_emit; ++frame) {
            for (int ch = 0; ch < channels; ++ch) {
                dest[frame * channels + ch] = static_cast<float>(emitted + frame + 1);
            }
        }
        emitted += frames_to_emit;
        return frames_to_emit;
    });

    reader.start(1, 2048);
    REQUIRE(wait_until_finished_with_frames(reader, 3));

    float buf[5] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    int got = reader.read(buf, 5, 1);
    REQUIRE(got == 3);
    REQUIRE(buf[0] == 1.0f);
    REQUIRE(buf[1] == 2.0f);
    REQUIRE(buf[2] == 3.0f);
    REQUIRE(buf[3] == 0.0f);
    REQUIRE(buf[4] == 0.0f);

    reader.stop();
}

TEST_CASE("BufferingReader preserves sample order across ring wrap",
          "[audio][buffering][issue-640]") {
    BufferingReader reader;

    int next_sample = 0;
    reader.set_read_callback([&](float* dest, int frames, int channels) {
        (void)channels;
        for (int frame = 0; frame < frames; ++frame) {
            dest[frame] = static_cast<float>(next_sample++);
        }
        return frames;
    });

    reader.start(1, 1100);
    REQUIRE(wait_for_frames(reader, 1000));

    std::vector<float> first(700, -1.0f);
    REQUIRE(reader.read(first.data(), static_cast<int>(first.size()), 1) == 700);
    REQUIRE(first.front() == 0.0f);
    REQUIRE(first.back() == 699.0f);

    REQUIRE(wait_for_frames(reader, 1000));

    std::vector<float> second(800, -1.0f);
    REQUIRE(reader.read(second.data(), static_cast<int>(second.size()), 1) == 800);
    for (int i = 0; i < static_cast<int>(second.size()); ++i) {
        REQUIRE(second[static_cast<size_t>(i)] == static_cast<float>(700 + i));
    }

    reader.stop();
}

TEST_CASE("BufferingReader signals finished when source ends", "[audio][buffering]") {
    BufferingReader reader;

    reader.set_read_callback([](float* dest, int frames, int) {
        // Return 0 to signal end of source
        (void)dest;
        (void)frames;
        return 0;
    });

    reader.start(1, 1024);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.is_finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(reader.is_finished());
    reader.stop();
}

TEST_CASE("BufferingReader without callback finishes and zero-fills reads",
          "[audio][buffering][edge]") {
    BufferingReader reader;

    reader.start(2, 1024);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.is_finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(reader.is_finished());

    float buf[8];
    std::fill(std::begin(buf), std::end(buf), -1.0f);
    REQUIRE(reader.read(buf, 4, 2) == 0);
    for (float sample : buf) {
        REQUIRE(sample == 0.0f);
    }

    reader.stop();
}

TEST_CASE("BufferingReader stereo interleaved", "[audio][buffering]") {
    BufferingReader reader;

    reader.set_read_callback([](float* dest, int frames, int channels) {
        for (int i = 0; i < frames; ++i) {
            dest[i * channels] = 0.5f;      // left
            dest[i * channels + 1] = -0.5f;  // right
        }
        return frames;
    });

    reader.start(2, 4096);

    // Wait for background thread to fill buffer (up to 2s)
    auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reader.frames_available() == 0 && std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    float buf[512]; // 256 frames * 2 channels
    int got = reader.read(buf, 256, 2);
    REQUIRE(got > 0);

    // Verify interleaved pattern
    REQUIRE_THAT(buf[0], WithinAbs(0.5, 0.001));  // left
    REQUIRE_THAT(buf[1], WithinAbs(-0.5, 0.001));  // right

    reader.stop();
}

TEST_CASE("BufferingReader read returns 0 on empty buffer", "[audio][buffering]") {
    BufferingReader reader;

    // Don't set callback — buffer stays empty
    reader.set_read_callback([](float*, int, int) { return 0; });
    reader.start(1, 1024);

    // Immediately try to read before background has time to fill
    float buf[64];
    int got = reader.read(buf, 64, 1);
    // May or may not have data yet — just verify no crash
    REQUIRE(got >= 0);

    reader.stop();
}

TEST_CASE("BufferingReader stop is safe to call multiple times", "[audio][buffering]") {
    BufferingReader reader;
    reader.set_read_callback([](float* dest, int frames, int) {
        for (int i = 0; i < frames; ++i) dest[i] = 0;
        return frames;
    });
    reader.start(1, 1024);
    reader.stop();
    reader.stop(); // double stop — should be safe
    REQUIRE_FALSE(reader.is_running());
}

TEST_CASE("BufferingReader channel mismatch returns 0", "[audio][buffering]") {
    BufferingReader reader;
    reader.set_read_callback([](float* dest, int frames, int) {
        for (int i = 0; i < frames; ++i) dest[i] = 1.0f;
        return frames;
    });
    reader.start(1, 1024);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    float buf[64];
    int got = reader.read(buf, 32, 2); // wrong channel count
    REQUIRE(got == 0);

    reader.stop();
}
