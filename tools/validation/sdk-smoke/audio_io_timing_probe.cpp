#include <pulp/audio/device.hpp>

namespace {

class UnsupportedTimingDevice final : public pulp::audio::AudioDevice {
public:
    bool open(const pulp::audio::DeviceConfig&) override { return true; }
    void close() override {}
    bool start(pulp::audio::AudioCallback) override { return true; }
    void stop() override {}
    bool is_open() const override { return true; }
    bool is_running() const override { return false; }
    pulp::audio::DeviceInfo info() const override { return {}; }
    double sample_rate() const override { return 48'000.0; }
    int buffer_size() const override { return 128; }
};

} // namespace

int main() {
    UnsupportedTimingDevice device;
    return pulp::audio::query_audio_io_timing(device).has_value() ? 1 : 0;
}
