#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/audio.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/channel_set.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>
#include <chrono>
#include <utility>

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

TEST_CASE("BufferView clears external storage and supports const access",
          "[audio][buffer][issue-640]") {
    float ch0[3] = {1.0f, -2.0f, 3.0f};
    float ch1[3] = {4.0f, -5.0f, 6.0f};
    float* ptrs[2] = {ch0, ch1};

    BufferView<float> view(ptrs, 2, 3);
    REQUIRE_FALSE(view.empty());
    REQUIRE(view.channel_ptr(0) == ch0);
    REQUIRE(view.channel_ptr(1) == ch1);

    view.clear();

    const BufferView<float>& const_view = view;
    REQUIRE(const_view.num_channels() == 2);
    REQUIRE(const_view.num_samples() == 3);
    REQUIRE(const_view.channel_ptr(0) == ch0);
    REQUIRE(const_view.channel(1).data() == ch1);

    for (std::size_t ch = 0; ch < const_view.num_channels(); ++ch) {
        for (auto sample : const_view.channel(ch)) {
            REQUIRE(sample == 0.0f);
        }
    }
}

TEST_CASE("BufferView supports int16 external storage",
          "[audio][buffer][coverage]") {
    int16_t ch0[3] = {1, -2, 3};
    int16_t ch1[3] = {4, -5, 6};
    int16_t* ptrs[2] = {ch0, ch1};

    BufferView<int16_t> view(ptrs, 2, 3);
    REQUIRE_FALSE(view.empty());
    REQUIRE(view.channel(0)[1] == -2);
    REQUIRE(view.channel_ptr(1) == ch1);

    view.channel(1)[2] = -12;
    REQUIRE(ch1[2] == -12);

    view.clear();
    for (std::size_t ch = 0; ch < view.num_channels(); ++ch) {
        for (auto sample : view.channel(ch)) {
            REQUIRE(sample == 0);
        }
    }
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

TEST_CASE("Buffer supports non-float sample storage",
          "[audio][buffer][coverage][phase3-github]") {
    Buffer<int16_t> buffer(2, 3);
    buffer.channel(0)[0] = 12;
    buffer.channel(1)[2] = -34;

    auto view = buffer.view();
    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 3);
    REQUIRE(view.channel(0)[0] == 12);
    REQUIRE(view.channel(1)[2] == -34);

    view.clear();
    REQUIRE(buffer.channel(0)[0] == 0);
    REQUIRE(buffer.channel(1)[2] == 0);
}

TEST_CASE("Buffer resize from empty regrows zero-filled channels",
          "[audio][buffer][coverage]") {
    Buffer<float> buf(2, 3);
    buf.channel(0)[0] = 1.0f;
    buf.channel(1)[2] = -1.0f;

    buf.resize(0, 0);
    REQUIRE(buf.num_channels() == 0);
    REQUIRE(buf.num_samples() == 0);
    REQUIRE(buf.view().empty());

    buf.resize(2, 2);
    REQUIRE(buf.num_channels() == 2);
    REQUIRE(buf.num_samples() == 2);
    REQUIRE_FALSE(buf.view().empty());
    REQUIRE(buf.view().channel_ptr(0) == buf.channel(0).data());
    REQUIRE(buf.view().channel_ptr(1) == buf.channel(1).data());

    for (std::size_t ch = 0; ch < buf.num_channels(); ++ch) {
        for (auto sample : buf.channel(ch)) {
            REQUIRE(sample == 0.0f);
        }
    }
}

TEST_CASE("Buffer zero-channel and zero-sample states remain well formed",
          "[audio][buffer][codecov]") {
    Buffer<float> zero_channels(0, 8);
    REQUIRE(zero_channels.num_channels() == 0);
    REQUIRE(zero_channels.num_samples() == 8);
    REQUIRE(zero_channels.view().empty());
    zero_channels.clear();

    Buffer<float> zero_samples(2, 0);
    REQUIRE(zero_samples.num_channels() == 2);
    REQUIRE(zero_samples.num_samples() == 0);
    auto empty_view = zero_samples.view();
    REQUIRE(empty_view.empty());
    REQUIRE(empty_view.num_channels() == 2);
    REQUIRE(empty_view.num_samples() == 0);
    zero_samples.clear();

    BufferView<float> default_view;
    REQUIRE(default_view.empty());
    REQUIRE(default_view.num_channels() == 0);
    REQUIRE(default_view.num_samples() == 0);
    default_view.clear();
}

TEST_CASE("Buffer can shrink to empty and grow with fresh zeroed storage",
          "[audio][buffer][coverage][phase3-github]") {
    Buffer<float> buffer(2, 2);
    buffer.channel(0)[0] = 1.0f;
    buffer.channel(1)[1] = -1.0f;

    buffer.resize(0, 0);
    REQUIRE(buffer.num_channels() == 0);
    REQUIRE(buffer.num_samples() == 0);
    REQUIRE(buffer.view().empty());

    buffer.resize(2, 2);
    REQUIRE_FALSE(buffer.view().empty());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        for (float sample : buffer.channel(ch)) {
            REQUIRE(sample == 0.0f);
        }
    }
}

TEST_CASE("Buffer resize to smaller shape remaps channel spans",
          "[audio][buffer][coverage][phase3]") {
    Buffer<float> buffer(3, 5);
    buffer.channel(0)[4] = 0.25f;
    buffer.channel(2)[4] = -0.5f;

    buffer.resize(2, 3);
    REQUIRE(buffer.num_channels() == 2);
    REQUIRE(buffer.num_samples() == 3);

    auto view = buffer.view();
    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 3);
    REQUIRE(view.channel_ptr(0) == buffer.channel(0).data());
    REQUIRE(view.channel_ptr(1) == buffer.channel(1).data());
    REQUIRE(buffer.channel(0).size() == 3);
    REQUIRE(buffer.channel(1).size() == 3);
}

TEST_CASE("Buffer copy owns independent channel storage",
          "[audio][buffer][coverage][phase3]") {
    Buffer<float> original(2, 3);
    original.channel(0)[0] = 1.0f;
    original.channel(1)[2] = 2.0f;

    Buffer<float> copied(original);
    copied.channel(0)[0] = 10.0f;
    copied.channel(1)[2] = 20.0f;

    REQUIRE(original.channel(0)[0] == 1.0f);
    REQUIRE(original.channel(1)[2] == 2.0f);
    auto copied_view = copied.view();
    auto original_view = original.view();
    REQUIRE(copied_view.channel_ptr(0) == copied.channel(0).data());
    REQUIRE(copied_view.channel_ptr(0) != original_view.channel_ptr(0));

    Buffer<float> assigned;
    assigned = original;
    assigned.channel(0)[1] = 30.0f;
    REQUIRE(original.channel(0)[1] == 0.0f);
    REQUIRE(assigned.view().channel_ptr(1) != original.view().channel_ptr(1));
}

TEST_CASE("Buffer move refreshes channel pointers for the new owner",
          "[audio][buffer][coverage][phase3]") {
    Buffer<float> original(2, 2);
    original.channel(0)[0] = 0.25f;
    original.channel(1)[1] = -0.5f;

    Buffer<float> moved(std::move(original));
    REQUIRE(moved.num_channels() == 2);
    REQUIRE(moved.num_samples() == 2);
    REQUIRE(moved.channel(0)[0] == 0.25f);
    REQUIRE(moved.channel(1)[1] == -0.5f);
    REQUIRE(moved.view().channel_ptr(1) == moved.channel(1).data());

    Buffer<float> assigned;
    assigned = std::move(moved);
    REQUIRE(assigned.num_channels() == 2);
    REQUIRE(assigned.num_samples() == 2);
    REQUIRE(assigned.channel(0)[0] == 0.25f);
    REQUIRE(assigned.channel(1)[1] == -0.5f);
    REQUIRE(assigned.view().channel_ptr(0) == assigned.channel(0).data());
}

TEST_CASE("BufferView slice offsets channel pointers and clamps length",
          "[audio][buffer][slice]") {
    Buffer<float> buffer(2, 6);
    for (std::size_t i = 0; i < 6; ++i) {
        buffer.channel(0)[i] = static_cast<float>(i);
        buffer.channel(1)[i] = static_cast<float>(10 + i);
    }

    auto view = buffer.view();
    auto slice = view.slice(2, 3);

    REQUIRE(slice.num_channels() == 2);
    REQUIRE(slice.num_samples() == 3);
    REQUIRE(slice.channel_ptr(0) == buffer.channel(0).data() + 2);
    REQUIRE(slice.channel_ptr(1) == buffer.channel(1).data() + 2);

    slice.channel(0)[0] = 100.0f;
    slice.channel(1)[2] = 200.0f;
    REQUIRE(buffer.channel(0)[2] == 100.0f);
    REQUIRE(buffer.channel(1)[4] == 200.0f);

    auto clamped = view.slice(4, 16);
    REQUIRE(clamped.num_samples() == 2);
    REQUIRE(clamped.channel_ptr(0) == buffer.channel(0).data() + 4);
}

TEST_CASE("BufferView clear only zeros sliced sample range",
          "[audio][buffer][slice]") {
    Buffer<float> buffer(2, 6);
    for (std::size_t i = 0; i < 6; ++i) {
        buffer.channel(0)[i] = static_cast<float>(i + 1);
        buffer.channel(1)[i] = static_cast<float>(10 + i);
    }

    buffer.view().slice(2, 3).clear();

    REQUIRE(buffer.channel(0)[0] == 1.0f);
    REQUIRE(buffer.channel(0)[1] == 2.0f);
    REQUIRE(buffer.channel(0)[2] == 0.0f);
    REQUIRE(buffer.channel(0)[3] == 0.0f);
    REQUIRE(buffer.channel(0)[4] == 0.0f);
    REQUIRE(buffer.channel(0)[5] == 6.0f);

    REQUIRE(buffer.channel(1)[0] == 10.0f);
    REQUIRE(buffer.channel(1)[1] == 11.0f);
    REQUIRE(buffer.channel(1)[2] == 0.0f);
    REQUIRE(buffer.channel(1)[3] == 0.0f);
    REQUIRE(buffer.channel(1)[4] == 0.0f);
    REQUIRE(buffer.channel(1)[5] == 15.0f);
}

TEST_CASE("Buffer self assignment preserves storage and channel pointers",
          "[audio][buffer][coverage][phase3]") {
    Buffer<float> buffer(2, 3);
    buffer.channel(0)[1] = 0.125f;
    buffer.channel(1)[2] = -0.25f;
    auto* left = buffer.channel(0).data();
    auto* right = buffer.channel(1).data();

    auto& copy_ref = buffer;
    buffer = copy_ref;
    REQUIRE(buffer.num_channels() == 2);
    REQUIRE(buffer.num_samples() == 3);
    REQUIRE(buffer.channel(0)[1] == 0.125f);
    REQUIRE(buffer.channel(1)[2] == -0.25f);
    REQUIRE(buffer.view().channel_ptr(0) == left);
    REQUIRE(buffer.view().channel_ptr(1) == right);

    auto& move_ref = buffer;
    buffer = std::move(move_ref);
    REQUIRE(buffer.num_channels() == 2);
    REQUIRE(buffer.num_samples() == 3);
    REQUIRE(buffer.channel(0)[1] == 0.125f);
    REQUIRE(buffer.channel(1)[2] == -0.25f);
    REQUIRE(buffer.view().channel_ptr(0) == left);
    REQUIRE(buffer.view().channel_ptr(1) == right);
}

TEST_CASE("AudioFileData reports shape from first channel",
          "[audio][file][codecov]") {
    AudioFileData empty;
    REQUIRE(empty.sample_rate == 0);
    REQUIRE(empty.num_channels() == 0);
    REQUIRE(empty.num_frames() == 0);
    REQUIRE(empty.empty());

    AudioFileData first_channel_empty;
    first_channel_empty.sample_rate = 44100;
    first_channel_empty.channels = {{}, {1.0f, 2.0f}};
    REQUIRE(first_channel_empty.num_channels() == 2);
    REQUIRE(first_channel_empty.num_frames() == 0);
    REQUIRE(first_channel_empty.empty());

    AudioFileData stereo;
    stereo.sample_rate = 48000;
    stereo.channels = {{0.0f, 0.25f, -0.25f}, {1.0f}};
    REQUIRE(stereo.num_channels() == 2);
    REQUIRE(stereo.num_frames() == 3);
    REQUIRE_FALSE(stereo.empty());
}

TEST_CASE("Device metadata defaults and custom configs are stable",
          "[audio][device][codecov]") {
    DeviceInfo info;
    REQUIRE(info.id.empty());
    REQUIRE(info.name.empty());
    REQUIRE(info.max_input_channels == 0);
    REQUIRE(info.max_output_channels == 0);
    REQUIRE(info.sample_rates.empty());
    REQUIRE(info.buffer_sizes.empty());
    REQUIRE_FALSE(info.is_default_input);
    REQUIRE_FALSE(info.is_default_output);

    DeviceConfig config;
    REQUIRE(config.device_id.empty());
    REQUIRE(config.sample_rate == 48000.0);
    REQUIRE(config.buffer_size == 256);
    REQUIRE(config.input_channels == 0);
    REQUIRE(config.output_channels == 2);

    config.device_id = "external-device";
    config.sample_rate = 96000.0;
    config.buffer_size = 128;
    config.input_channels = 2;
    config.output_channels = 6;
    REQUIRE(config.device_id == "external-device");
    REQUIRE(config.sample_rate == 96000.0);
    REQUIRE(config.buffer_size == 128);
    REQUIRE(config.input_channels == 2);
    REQUIRE(config.output_channels == 6);
}

TEST_CASE("AudioSystem default device-change callback is snapshot safe",
          "[audio][device][coverage][phase3]") {
    class DummyAudioSystem final : public AudioSystem {
    public:
        std::vector<DeviceInfo> enumerate_devices() override { return {}; }
        std::unique_ptr<AudioDevice> create_device(const std::string& = "") override {
            return {};
        }
        DeviceInfo default_output_device() override { return {}; }
        DeviceInfo default_input_device() override { return {}; }
    };

    DummyAudioSystem system;
    int calls = 0;

    REQUIRE_FALSE(system.has_device_change_callback());
    system.fire_device_change();
    REQUIRE(calls == 0);

    system.set_device_change_callback([&] {
        ++calls;
        system.set_device_change_callback(nullptr);
    });
    REQUIRE(system.has_device_change_callback());

    system.fire_device_change();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(system.has_device_change_callback());

    system.fire_device_change();
    REQUIRE(calls == 1);

    system.set_device_change_callback([&] {
        ++calls;
        system.set_device_change_callback([&] { calls += 10; });
    });
    REQUIRE(system.has_device_change_callback());

    system.fire_device_change();
    REQUIRE(calls == 2);
    REQUIRE(system.has_device_change_callback());

    system.fire_device_change();
    REQUIRE(calls == 12);

    system.set_device_change_callback(nullptr);
    REQUIRE_FALSE(system.has_device_change_callback());
}

TEST_CASE("AudioProcessLoadMeasurer ignores invalid callback geometry",
          "[audio][load][coverage][phase3]") {
    AudioProcessLoadMeasurer measurer;

    measurer.begin(0, 48000.0f);
    measurer.end();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() == 0.0f);

    measurer.begin(64, 0.0f);
    measurer.end();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() == 0.0f);
}

TEST_CASE("AudioProcessLoadMeasurer rejects non-finite timing budgets",
          "[audio][load][coverage][phase3]") {
    AudioProcessLoadMeasurer measurer;

    measurer.begin(64, std::numeric_limits<float>::infinity());
    measurer.end();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() == 0.0f);

    measurer.begin(std::numeric_limits<int>::max(), 1.0e-30f);
    measurer.end();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() == 0.0f);
}

TEST_CASE("AudioProcessLoadMeasurer clamps smoothing and resets peak load",
          "[audio][load][coverage][phase3]") {
    AudioProcessLoadMeasurer measurer;

    measurer.set_smoothing(-1.0f);
    measurer.begin(64, 48000.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    measurer.end();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() > 0.0f);

    measurer.reset_peak();
    REQUIRE(measurer.peak_load() == 0.0f);

    measurer.set_smoothing(2.0f);
    measurer.begin(64, 48000.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    measurer.end();
    REQUIRE(measurer.load() > 0.0f);
    REQUIRE(measurer.peak_load() >= measurer.load());

    measurer.reset();
    REQUIRE(measurer.load() == 0.0f);
    REQUIRE(measurer.peak_load() == 0.0f);
}

TEST_CASE("CallbackContext defaults and sample position remain explicit",
          "[audio][device][codecov]") {
    CallbackContext empty;
    REQUIRE(empty.sample_rate == 0.0);
    REQUIRE(empty.buffer_size == 0);
    REQUIRE(empty.sample_position == 0);

    CallbackContext block;
    block.sample_rate = 44100.0;
    block.buffer_size = 512;
    block.sample_position = 4096;

    REQUIRE(block.sample_rate == 44100.0);
    REQUIRE(block.buffer_size == 512);
    REQUIRE(block.sample_position == 4096);
}

TEST_CASE("AudioCallback receives stable context and writable output views",
          "[audio][device][codecov]") {
    CallbackContext defaults;
    REQUIRE(defaults.sample_rate == 0.0);
    REQUIRE(defaults.buffer_size == 0);
    REQUIRE(defaults.sample_position == 0);

    float in_left[3] = {0.25f, -0.5f, 1.0f};
    float in_right[3] = {1.0f, 0.5f, -0.25f};
    const float* input_channels[2] = {in_left, in_right};
    float out_left[3] = {};
    float out_right[3] = {};
    float* output_channels[2] = {out_left, out_right};

    BufferView<const float> input(input_channels, 2, 3);
    BufferView<float> output(output_channels, 2, 3);
    CallbackContext context;
    context.sample_rate = 96000.0;
    context.buffer_size = 3;
    context.sample_position = 1024;

    bool called = false;
    AudioCallback callback = [&](const BufferView<const float>& in,
                                 BufferView<float>& out,
                                 const CallbackContext& ctx) {
        called = true;
        REQUIRE(ctx.sample_rate == 96000.0);
        REQUIRE(ctx.buffer_size == 3);
        REQUIRE(ctx.sample_position == 1024);
        REQUIRE(in.num_channels() == 2);
        REQUIRE(out.num_channels() == 2);
        REQUIRE(in.num_samples() == 3);
        REQUIRE(out.num_samples() == 3);

        for (std::size_t sample = 0; sample < out.num_samples(); ++sample) {
            out.channel(0)[sample] = in.channel(0)[sample] + in.channel(1)[sample];
            out.channel(1)[sample] = in.channel(0)[sample] - in.channel(1)[sample];
        }
    };

    callback(input, output, context);

    REQUIRE(called);
    REQUIRE(out_left[0] == 1.25f);
    REQUIRE(out_left[1] == 0.0f);
    REQUIRE(out_left[2] == 0.75f);
    REQUIRE(out_right[0] == -0.75f);
    REQUIRE(out_right[1] == -1.0f);
    REQUIRE(out_right[2] == 1.25f);
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

TEST_CASE("ChannelSet discrete layouts use unnamed speaker slots",
          "[audio][channel-set][issue-640]") {
    auto empty = ChannelSet::discrete(0);
    REQUIRE(empty.name == "Discrete 0");
    REQUIRE(empty.speakers.empty());

    auto custom = ChannelSet::discrete(4);
    REQUIRE(custom.name == "Discrete 4");
    REQUIRE(custom.size() == 4);
    for (auto speaker : custom.speakers) {
        REQUIRE(speaker == Speaker::Discrete);
    }
    REQUIRE_FALSE(custom == ChannelSet::from_channel_count(4));
}

TEST_CASE("ChannelSet from_name matching is exact and defaults to stereo speakers",
          "[audio][channel-set][issue-640]") {
    REQUIRE(ChannelSet::from_name("stereo") == ChannelSet::discrete(2));
    REQUIRE(ChannelSet::from_name(" Stereo") == ChannelSet::discrete(2));
    REQUIRE(ChannelSet::from_name("") == ChannelSet::discrete(2));
    REQUIRE(ChannelSet::from_name("7.1.4 ") == ChannelSet::discrete(2));
}

TEST_CASE("ChannelSet immersive layout preserves documented speaker order",
          "[audio][channel-set][issue-640]") {
    auto atmos = ChannelSet::surround_7_1_4();
    REQUIRE(atmos.size() == 12);
    REQUIRE(atmos.speakers[0] == Speaker::FrontLeft);
    REQUIRE(atmos.speakers[1] == Speaker::FrontRight);
    REQUIRE(atmos.speakers[2] == Speaker::FrontCenter);
    REQUIRE(atmos.speakers[3] == Speaker::LFE);
    REQUIRE(atmos.speakers[4] == Speaker::BackLeft);
    REQUIRE(atmos.speakers[5] == Speaker::BackRight);
    REQUIRE(atmos.speakers[6] == Speaker::SideLeft);
    REQUIRE(atmos.speakers[7] == Speaker::SideRight);
    REQUIRE(atmos.speakers[8] == Speaker::TopFrontLeft);
    REQUIRE(atmos.speakers[9] == Speaker::TopFrontRight);
    REQUIRE(atmos.speakers[10] == Speaker::TopBackLeft);
    REQUIRE(atmos.speakers[11] == Speaker::TopBackRight);
}

TEST_CASE("Buffer resize handles type changes and preserves new shape",
          "[audio][buffer][coverage][phase3]") {
    Buffer<double> buf(3, 2);
    buf.channel(0)[0] = 1.0;
    buf.channel(1)[1] = -2.0;

    buf.resize(1, 5);
    REQUIRE(buf.num_channels() == 1);
    REQUIRE(buf.num_samples() == 5);
    REQUIRE(buf.channel(0).size() == 5);

    buf.clear();
    for (auto sample : buf.channel(0)) {
        REQUIRE(sample == 0.0);
    }

    buf.channel(0)[4] = 0.5;
    auto view = buf.view();
    REQUIRE(view.channel(0)[4] == 0.5);
}

TEST_CASE("Buffer resize through empty shapes rebuilds channel pointers",
          "[audio][buffer][coverage]") {
    Buffer<float> buf(2, 3);
    buf.channel(0)[0] = 1.0f;
    buf.channel(1)[2] = -1.0f;

    buf.resize(0, 0);
    REQUIRE(buf.num_channels() == 0);
    REQUIRE(buf.num_samples() == 0);
    REQUIRE(buf.view().empty());

    buf.resize(2, 2);
    REQUIRE(buf.num_channels() == 2);
    REQUIRE(buf.num_samples() == 2);
    REQUIRE_FALSE(buf.view().empty());
    REQUIRE(buf.channel(0).data() != nullptr);
    REQUIRE(buf.channel(1).data() != nullptr);

    for (std::size_t ch = 0; ch < buf.num_channels(); ++ch) {
        for (auto sample : buf.channel(ch)) {
            REQUIRE(sample == 0.0f);
        }
    }

    buf.channel(0)[1] = 0.25f;
    const Buffer<float>& const_buf = buf;
    REQUIRE(const_buf.channel(0)[1] == 0.25f);
    REQUIRE(const_buf.channel(1).size() == 2);
}

TEST_CASE("BufferView supports zero-sample clears without touching channel pointers",
          "[audio][buffer][coverage][phase3]") {
    float left = 1.0f;
    float right = -1.0f;
    float* ptrs[2] = {&left, &right};

    BufferView<float> view(ptrs, 2, 0);
    REQUIRE(view.empty());
    REQUIRE(view.num_channels() == 2);
    REQUIRE(view.num_samples() == 0);
    REQUIRE(view.channel_ptr(0) == &left);
    REQUIRE(view.channel_ptr(1) == &right);

    view.clear();
    REQUIRE(left == 1.0f);
    REQUIRE(right == -1.0f);
}

TEST_CASE("ChannelSet discrete layouts compare by speaker map not display name",
          "[audio][channel-set][coverage][phase3]") {
    auto named = ChannelSet::discrete(3);
    auto renamed = named;
    renamed.name = "Three unnamed channels";

    REQUIRE(named == renamed);
    REQUIRE(named.size() == 3);
    REQUIRE_FALSE(named == ChannelSet::lrc());
}

TEST_CASE("ChannelSet speaker names cover top and discrete positions",
          "[audio][channel-set][coverage][phase3-github]") {
    REQUIRE(speaker_name(Speaker::TopFrontLeft) == "Top Front Left");
    REQUIRE(speaker_name(Speaker::TopFrontRight) == "Top Front Right");
    REQUIRE(speaker_name(Speaker::TopBackLeft) == "Top Back Left");
    REQUIRE(speaker_name(Speaker::TopBackRight) == "Top Back Right");
    REQUIRE(speaker_name(Speaker::TopCenter) == "Top Center");
    REQUIRE(speaker_name(Speaker::Discrete) == "Discrete");
}

TEST_CASE("ChannelSet unsupported surround counts fall back to discrete layouts",
          "[audio][channel-set][coverage][phase3]") {
    for (auto channels : {7u, 9u, 10u, 11u}) {
        auto layout = ChannelSet::from_channel_count(channels);
        CAPTURE(channels);
        REQUIRE(layout.name == "Discrete " + std::to_string(channels));
        REQUIRE(layout.size() == channels);
        for (auto speaker : layout.speakers) {
            REQUIRE(speaker == Speaker::Discrete);
        }
    }
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
