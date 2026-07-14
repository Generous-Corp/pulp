// Item 4.5 — typed plugin introspection.
// Verifies NativeHandleVisitor double-dispatches into the correct visit_*
// method and surfaces a populated *Extension struct for known formats.
// Slots that the running build did not link an SDK against (e.g. AU on a
// Linux CI lane) fall through to visit_unknown, which is exactly the
// contract documented in native_handle_visitor.hpp.

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/native_handle_visitor.hpp>
#include <pulp/host/plugin_slot.hpp>

using namespace pulp::host;

namespace {

// Minimal slot that does not override accept(); should fall through to
// the base PluginSlot::accept which calls visit_unknown.
class StubSlot final : public PluginSlot {
public:
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int) override {}
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_{};
};

// Slot that pretends to be a CLAP plugin so the visitor sees a CLAP
// dispatch path with a non-null handle.
class FakeClapSlot final : public PluginSlot {
public:
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int) override {}
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    void accept(NativeHandleVisitor& v) const override {
        ClapNativeHandle ext;
        // Sentinel pointers — the test only checks they survive the
        // dispatch unchanged. Real ClapSlot fills these with the
        // `const clap_plugin_t*` / `const clap_host_t*` it owns.
        ext.plugin = reinterpret_cast<void*>(0xC1ABDEAD);
        ext.host = reinterpret_cast<void*>(0xC1ABCAFE);
        ext.plugin_id = "com.pulp.test.clap";
        v.visit_clap(*this, ext);
    }

private:
    PluginInfo info_{};
};

struct CapturingVisitor : NativeHandleVisitor {
    enum class Path { None, Unknown, Vst3, Au, AuV3, Clap, Lv2 };
    Path path = Path::None;
    NativeHandleFormat unknown_format = NativeHandleFormat::Unknown;
    Vst3NativeHandle vst3{};
    AudioUnitNativeHandle au{};
    AudioUnitV3NativeHandle auv3{};
    ClapNativeHandle clap{};
    Lv2NativeHandle lv2{};

    void visit_unknown(const PluginSlot&, NativeHandleFormat f) override {
        path = Path::Unknown;
        unknown_format = f;
    }
    void visit_vst3(const PluginSlot&, const Vst3NativeHandle& e) override {
        path = Path::Vst3;
        vst3 = e;
    }
    void visit_audio_unit(const PluginSlot&, const AudioUnitNativeHandle& e) override {
        path = Path::Au;
        au = e;
    }
    void visit_audio_unit_v3(const PluginSlot&, const AudioUnitV3NativeHandle& e) override {
        path = Path::AuV3;
        auv3 = e;
    }
    void visit_clap(const PluginSlot&, const ClapNativeHandle& e) override {
        path = Path::Clap;
        clap = e;
    }
    void visit_lv2(const PluginSlot&, const Lv2NativeHandle& e) override {
        path = Path::Lv2;
        lv2 = e;
    }
};

} // namespace

TEST_CASE("NativeHandleVisitor base slot dispatches to visit_unknown",
          "[host][extensions-visitor]") {
    StubSlot slot;
    CapturingVisitor v;
    slot.accept(v);
    REQUIRE(v.path == CapturingVisitor::Path::Unknown);
    REQUIRE(v.unknown_format == NativeHandleFormat::Unknown);
}

TEST_CASE("NativeHandleVisitor CLAP slot dispatches to visit_clap with handles",
          "[host][extensions-visitor][clap]") {
    FakeClapSlot slot;
    CapturingVisitor v;
    slot.accept(v);
    REQUIRE(v.path == CapturingVisitor::Path::Clap);
    REQUIRE(v.clap.plugin == reinterpret_cast<void*>(0xC1ABDEAD));
    REQUIRE(v.clap.host == reinterpret_cast<void*>(0xC1ABCAFE));
    REQUIRE(v.clap.plugin_id == "com.pulp.test.clap");
}

// A visitor that only overrides visit_unknown should still receive a
// notification when a typed visit_* fires, because the default visit_*
// fall through to visit_unknown.
TEST_CASE("NativeHandleVisitor unhandled format paths fall through to visit_unknown",
          "[host][extensions-visitor]") {
    struct UnknownOnlyVisitor : NativeHandleVisitor {
        NativeHandleFormat seen = NativeHandleFormat::Unknown;
        int unknown_count = 0;
        void visit_unknown(const PluginSlot&, NativeHandleFormat f) override {
            seen = f;
            ++unknown_count;
        }
    };

    FakeClapSlot slot;
    UnknownOnlyVisitor v;
    slot.accept(v);
    REQUIRE(v.unknown_count == 1);
    REQUIRE(v.seen == NativeHandleFormat::CLAP);
}

// The NativeHandleFormat enum exists so a visitor that overrides
// visit_unknown can disambiguate which adapter type funneled in. Confirm
// every documented enumerator survives a value round-trip; this acts as a
// guard against accidental reorder/removal that would silently break
// existing visitors.
TEST_CASE("NativeHandleVisitor NativeHandleFormat values are stable",
          "[host][extensions-visitor]") {
    REQUIRE(static_cast<int>(NativeHandleFormat::Unknown) == 0);
    REQUIRE(static_cast<int>(NativeHandleFormat::VST3) == 1);
    REQUIRE(static_cast<int>(NativeHandleFormat::AudioUnit) == 2);
    REQUIRE(static_cast<int>(NativeHandleFormat::AudioUnitV3) == 3);
    REQUIRE(static_cast<int>(NativeHandleFormat::CLAP) == 4);
    REQUIRE(static_cast<int>(NativeHandleFormat::LV2) == 5);
}
