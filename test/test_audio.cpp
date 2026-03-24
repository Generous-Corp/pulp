#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/audio.hpp>
#include <cmath>
#include <thread>
#include <chrono>

using namespace pulp::audio;

TEST_CASE("Buffer owning", "[audio][buffer]") {
    Buffer<float> buf(2, 256);

    SECTION("Correct dimensions") {
        REQUIRE(buf.num_channels() == 2);
        REQUIRE(buf.num_samples() == 256);
    }

    SECTION("Initialized to zero") {
        for (std::size_t ch = 0; ch < buf.num_channels(); ++ch) {
            for (auto sample : buf.channel(ch)) {
                REQUIRE(sample == 0.0f);
            }
        }
    }

    SECTION("Write and read back") {
        buf.channel(0)[0] = 1.0f;
        buf.channel(1)[255] = -1.0f;
        REQUIRE(buf.channel(0)[0] == 1.0f);
        REQUIRE(buf.channel(1)[255] == -1.0f);
    }

    SECTION("Clear") {
        buf.channel(0)[0] = 42.0f;
        buf.clear();
        REQUIRE(buf.channel(0)[0] == 0.0f);
    }
}

TEST_CASE("BufferView non-owning", "[audio][buffer]") {
    float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float ch1[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float* ptrs[2] = {ch0, ch1};

    BufferView<float> view(ptrs, 2, 4);

    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 4);
    REQUIRE(view.channel(0)[0] == 1.0f);
    REQUIRE(view.channel(1)[3] == 8.0f);

    // Modifications go through to original memory
    view.channel(0)[0] = 99.0f;
    REQUIRE(ch0[0] == 99.0f);
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
TEST_CASE("CoreAudio system enumerates devices", "[audio][coreaudio]") {
    auto system = create_audio_system();
    REQUIRE(system != nullptr);

    auto devices = system->enumerate_devices();
    REQUIRE_FALSE(devices.empty());

    // Should have at least one output device
    bool has_output = false;
    for (const auto& d : devices) {
        if (d.max_output_channels > 0) has_output = true;
    }
    REQUIRE(has_output);
}

TEST_CASE("CoreAudio default output device", "[audio][coreaudio]") {
    auto system = create_audio_system();
    auto info = system->default_output_device();

    REQUIRE_FALSE(info.name.empty());
    REQUIRE(info.max_output_channels > 0);
}

TEST_CASE("CoreAudio render sine wave", "[audio][coreaudio]") {
    auto system = create_audio_system();
    auto device = system->create_device();

    DeviceConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;

    REQUIRE(device->open(config));

    double phase = 0.0;
    const double freq = 440.0;
    int callbacks_received = 0;

    auto ok = device->start([&](const BufferView<const float>&,
                                 BufferView<float>& output,
                                 const CallbackContext& ctx) {
        const double inc = freq / ctx.sample_rate;
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            float sample = static_cast<float>(std::sin(phase * 2.0 * M_PI)) * 0.1f;
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
                output.channel(ch)[i] = sample;
            }
            phase += inc;
        }
        callbacks_received++;
    });

    REQUIRE(ok);

    // Let it run briefly (50ms ≈ 9 callbacks at 48kHz/256)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    device->stop();
    device->close();

    REQUIRE(callbacks_received > 0);
}
#endif
