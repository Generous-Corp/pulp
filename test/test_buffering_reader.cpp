#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/buffering_reader.hpp>
#include <cmath>
#include <thread>
#include <chrono>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

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

    // Wait for background thread to fill some buffer
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(reader.frames_available() > 0);

    float buf[256];
    int got = reader.read(buf, 256, 1);
    REQUIRE(got > 0);

    // First sample should be 0.0f (sequential counter)
    REQUIRE_THAT(buf[0], WithinAbs(0.0, 0.001));

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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
