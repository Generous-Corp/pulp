#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/vst3_adapter.hpp>

#include <cstring>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

class TestVst3Processor : public pulp::format::Processor {
public:
    TestVst3Processor() { g_last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Vst3PluginStateTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.vst3-plugin-state",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
    static TestVst3Processor* g_last_processor;
};

TestVst3Processor* TestVst3Processor::g_last_processor = nullptr;

std::unique_ptr<pulp::format::Processor> create_test_processor() {
    return std::make_unique<TestVst3Processor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return {};
}

class HostApp final : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        for (int i = 0; i < 127 && kName[i]; ++i) {
            name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        }
        name[8] = 0;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID,
                                                 Steinberg::TUID,
                                                 void** obj) override {
        if (obj) *obj = nullptr;
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

class VectorStream final : public Steinberg::IBStream {
public:
    VectorStream() = default;
    explicit VectorStream(const std::vector<uint8_t>& src) : buf_(src) {}

    std::vector<uint8_t> take() { return std::move(buf_); }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 num_bytes,
                                       Steinberg::int32* num_bytes_read) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        const Steinberg::int64 remaining =
            static_cast<Steinberg::int64>(buf_.size()) - pos_;
        const Steinberg::int64 count = num_bytes < remaining ? num_bytes : remaining;
        if (count > 0) {
            std::memcpy(buffer, buf_.data() + pos_, static_cast<std::size_t>(count));
        }
        pos_ += count;
        if (num_bytes_read) *num_bytes_read = static_cast<Steinberg::int32>(count);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 num_bytes,
                                        Steinberg::int32* num_bytes_written) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        buf_.insert(buf_.end(), bytes, bytes + num_bytes);
        pos_ = static_cast<Steinberg::int64>(buf_.size());
        if (num_bytes_written) *num_bytes_written = num_bytes;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override {
        Steinberg::int64 new_pos = pos_;
        switch (mode) {
            case kIBSeekSet:
                new_pos = pos;
                break;
            case kIBSeekCur:
                new_pos = pos_ + pos;
                break;
            case kIBSeekEnd:
                new_pos = static_cast<Steinberg::int64>(buf_.size()) + pos;
                break;
            default:
                return Steinberg::kInvalidArgument;
        }
        if (new_pos < 0 || new_pos > static_cast<Steinberg::int64>(buf_.size())) {
            return Steinberg::kInvalidArgument;
        }
        pos_ = new_pos;
        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = pos_;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::vector<uint8_t> buf_;
    Steinberg::int64 pos_ = 0;
};

} // namespace

TEST_CASE("VST3 getState/setState round-trip includes plugin-owned payload",
          "[vst3][state]") {
    HostApp host_app;

    pulp::format::vst3::PulpVst3Processor saver(create_test_processor);
    REQUIRE(saver.initialize(&host_app) == Steinberg::kResultOk);
    auto* saver_processor = TestVst3Processor::g_last_processor;
    REQUIRE(saver_processor != nullptr);
    saver_processor->state().set_value(1, -15.0f);
    saver_processor->plugin_state = "layout=64";

    VectorStream out_stream;
    REQUIRE(saver.getState(&out_stream) == Steinberg::kResultOk);
    auto saved = out_stream.take();
    REQUIRE(saved.size() >= 4);
    REQUIRE(saved[0] == 'P');
    REQUIRE(saved[1] == 'L');
    REQUIRE(saved[2] == 'S');
    REQUIRE(saved[3] == 'T');

    pulp::format::vst3::PulpVst3Processor loader(create_test_processor);
    REQUIRE(loader.initialize(&host_app) == Steinberg::kResultOk);
    auto* loader_processor = TestVst3Processor::g_last_processor;
    REQUIRE(loader_processor != nullptr);
    loader_processor->state().set_value(1, 9.0f);
    loader_processor->plugin_state = "stale";

    VectorStream in_stream(saved);
    REQUIRE(loader.setState(&in_stream) == Steinberg::kResultOk);
    REQUIRE_THAT(loader_processor->state().get_value(1), WithinAbs(-15.0, 0.01));
    REQUIRE(loader_processor->plugin_state == "layout=64");

    REQUIRE(loader.terminate() == Steinberg::kResultOk);
    REQUIRE(saver.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 setState rejects invalid plugin payload",
          "[vst3][state]") {
    HostApp host_app;

    pulp::format::vst3::PulpVst3Processor loader(create_test_processor);
    REQUIRE(loader.initialize(&host_app) == Steinberg::kResultOk);
    auto* processor = TestVst3Processor::g_last_processor;
    REQUIRE(processor != nullptr);
    processor->state().set_value(1, 7.0f);
    processor->plugin_state = "keep";

    VectorStream bad_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
    REQUIRE(loader.setState(&bad_stream) == Steinberg::kResultFalse);
    REQUIRE_THAT(processor->state().get_value(1), WithinAbs(7.0, 0.01));
    REQUIRE(processor->plugin_state == "keep");

    REQUIRE(loader.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 getState/setState fail cleanly without a live processor",
          "[vst3][state]") {
    HostApp host_app;

    SECTION("after terminate") {
        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);

        VectorStream out_stream;
        REQUIRE(processor.getState(&out_stream) == Steinberg::kResultFalse);

        VectorStream in_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
        REQUIRE(processor.setState(&in_stream) == Steinberg::kResultFalse);
    }

    SECTION("null factory") {
        pulp::format::vst3::PulpVst3Processor processor(create_null_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kInternalError);

        VectorStream out_stream;
        REQUIRE(processor.getState(&out_stream) == Steinberg::kResultFalse);

        VectorStream in_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
        REQUIRE(processor.setState(&in_stream) == Steinberg::kResultFalse);
    }
}
