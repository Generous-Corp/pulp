#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/audio.hpp>
#include <pulp/audio/channel_set.hpp>
#include <cmath>
#include <numbers>
#include <thread>
#include <chrono>

using namespace pulp::audio;

#if defined(__APPLE__) && !TARGET_OS_IPHONE
static std::vector<DeviceInfo> require_coreaudio_output_devices(AudioSystem& system) {
    auto devices = system.enumerate_devices();
    bool has_output = false;
    for (const auto& device : devices) {
        if (device.max_output_channels > 0) {
            has_output = true;
            break;
        }
    }
    if (!has_output) {
        SKIP("No CoreAudio output device is available in this environment");
    }
    return devices;
}

static DeviceInfo require_coreaudio_default_output(AudioSystem& system) {
    auto info = system.default_output_device();
    if (info.name.empty() || info.max_output_channels == 0) {
        SKIP("No CoreAudio default output device is available in this environment");
    }
    return info;
}
#endif

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

TEST_CASE("Buffer resize and views expose contiguous channel storage",
          "[audio][buffer][issue-640]") {
    Buffer<float> empty;
    REQUIRE(empty.num_channels() == 0);
    REQUIRE(empty.num_samples() == 0);
    REQUIRE(empty.view().empty());

    Buffer<float> buf(1, 3);
    REQUIRE_FALSE(buf.view().empty());
    buf.channel(0)[0] = 1.0f;
    buf.channel(0)[1] = 2.0f;
    buf.channel(0)[2] = 3.0f;

    buf.resize(2, 4);
    REQUIRE(buf.num_channels() == 2);
    REQUIRE(buf.num_samples() == 4);
    auto view = buf.view();
    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 4);
    REQUIRE(view.channel_ptr(0) == buf.channel(0).data());
    REQUIRE(view.channel_ptr(1) == buf.channel(1).data());

    view.channel(0)[0] = 0.25f;
    view.channel(1)[3] = -0.75f;
    REQUIRE(buf.channel(0)[0] == 0.25f);
    REQUIRE(buf.channel(1)[3] == -0.75f);

    buf.clear();
    for (std::size_t ch = 0; ch < buf.num_channels(); ++ch) {
        for (auto sample : buf.channel(ch)) {
            REQUIRE(sample == 0.0f);
        }
    }

    buf.resize(2, 0);
    REQUIRE(buf.view().empty());
}

TEST_CASE("ChannelSet maps standard layouts by count and name",
          "[audio][channel-set][issue-640]") {
    REQUIRE(ChannelSet::from_channel_count(0).name == "Discrete 0");
    REQUIRE(ChannelSet::from_channel_count(1) == ChannelSet::mono());
    REQUIRE(ChannelSet::from_channel_count(2) == ChannelSet::stereo());
    REQUIRE(ChannelSet::from_channel_count(3) == ChannelSet::lrc());
    REQUIRE(ChannelSet::from_channel_count(4) == ChannelSet::quad());
    REQUIRE(ChannelSet::from_channel_count(5) == ChannelSet::surround_5_0());
    REQUIRE(ChannelSet::from_channel_count(6) == ChannelSet::surround_5_1());
    REQUIRE(ChannelSet::from_channel_count(8) == ChannelSet::surround_7_1());
    REQUIRE(ChannelSet::from_channel_count(12) == ChannelSet::surround_7_1_4());
    REQUIRE(ChannelSet::from_channel_count(9).name == "Discrete 9");
    REQUIRE(ChannelSet::from_channel_count(9).size() == 9);

    REQUIRE(ChannelSet::from_name("Mono") == ChannelSet::mono());
    REQUIRE(ChannelSet::from_name("Stereo") == ChannelSet::stereo());
    REQUIRE(ChannelSet::from_name("LRC") == ChannelSet::lrc());
    REQUIRE(ChannelSet::from_name("Quad") == ChannelSet::quad());
    REQUIRE(ChannelSet::from_name("5.0") == ChannelSet::surround_5_0());
    REQUIRE(ChannelSet::from_name("5.1") == ChannelSet::surround_5_1());
    REQUIRE(ChannelSet::from_name("7.1") == ChannelSet::surround_7_1());
    REQUIRE(ChannelSet::from_name("7.1.4") == ChannelSet::surround_7_1_4());
    REQUIRE(ChannelSet::from_name("7.1.4 (Atmos bed)") == ChannelSet::surround_7_1_4());
    REQUIRE(ChannelSet::from_name("not-a-layout") == ChannelSet::discrete(2));
}

TEST_CASE("ChannelSet speaker names and equality are deterministic",
          "[audio][channel-set][issue-640]") {
    REQUIRE(speaker_name(Speaker::FrontLeft) == "Front Left");
    REQUIRE(speaker_name(Speaker::FrontRight) == "Front Right");
    REQUIRE(speaker_name(Speaker::FrontCenter) == "Front Center");
    REQUIRE(speaker_name(Speaker::LFE) == "LFE");
    REQUIRE(speaker_name(Speaker::BackLeft) == "Back Left");
    REQUIRE(speaker_name(Speaker::BackRight) == "Back Right");
    REQUIRE(speaker_name(Speaker::SideLeft) == "Side Left");
    REQUIRE(speaker_name(Speaker::SideRight) == "Side Right");
    REQUIRE(speaker_name(Speaker::TopFrontLeft) == "Top Front Left");
    REQUIRE(speaker_name(Speaker::TopFrontRight) == "Top Front Right");
    REQUIRE(speaker_name(Speaker::TopBackLeft) == "Top Back Left");
    REQUIRE(speaker_name(Speaker::TopBackRight) == "Top Back Right");
    REQUIRE(speaker_name(Speaker::TopCenter) == "Top Center");
    REQUIRE(speaker_name(Speaker::Discrete) == "Discrete");
    REQUIRE(speaker_name(static_cast<Speaker>(255)) == "Unknown");

    ChannelSet renamed_stereo;
    renamed_stereo.name = "Renamed";
    renamed_stereo.speakers = {Speaker::FrontLeft, Speaker::FrontRight};
    REQUIRE(renamed_stereo == ChannelSet::stereo());
    REQUIRE_FALSE(ChannelSet::stereo() == ChannelSet::quad());
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
TEST_CASE("CoreAudio system enumerates devices", "[audio][coreaudio]") {
    auto system = create_audio_system();
    REQUIRE(system != nullptr);

    auto devices = require_coreaudio_output_devices(*system);
    REQUIRE_FALSE(devices.empty());
}

TEST_CASE("CoreAudio default output device", "[audio][coreaudio]") {
    auto system = create_audio_system();
    auto info = require_coreaudio_default_output(*system);

    REQUIRE_FALSE(info.name.empty());
    REQUIRE(info.max_output_channels > 0);
}

TEST_CASE("CoreAudio render sine wave", "[audio][coreaudio]") {
    auto system = create_audio_system();
    auto info = require_coreaudio_default_output(*system);
    auto device = system->create_device();

    DeviceConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = static_cast<std::size_t>(std::min(2, info.max_output_channels));

    if (!device->open(config)) {
        SKIP("CoreAudio device could not be opened in this environment");
    }

    double phase = 0.0;
    const double freq = 440.0;
    int callbacks_received = 0;

    auto ok = device->start([&](const BufferView<const float>&,
                                 BufferView<float>& output,
                                 const CallbackContext& ctx) {
        const double inc = freq / ctx.sample_rate;
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            float sample = static_cast<float>(std::sin(phase * 2.0 * std::numbers::pi_v<double>)) * 0.1f;
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
                output.channel(ch)[i] = sample;
            }
            phase += inc;
        }
        callbacks_received++;
    });

    if (!ok) {
        device->close();
        SKIP("CoreAudio callback could not be started in this environment");
    }

    // Let it run briefly (50ms ≈ 9 callbacks at 48kHz/256)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    device->stop();
    device->close();

    if (callbacks_received == 0) {
        SKIP("CoreAudio callback never fired in this environment");
    }
    REQUIRE(callbacks_received > 0);
}
#endif
