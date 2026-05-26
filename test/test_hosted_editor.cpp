// Hosted-editor API tests (workstream 03 slice 3.4). Verifies the typed
// wrapper and the legacy-compat path — existing slots that only override
// create_editor_view() still produce a valid HostedEditor.

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/plugin_slot.hpp>

using namespace pulp::host;

// The legacy `void* create_editor_view()` / `destroy_editor_view()` virtuals
// were marked `[[deprecated]]` in item 4.4 (macOS plan). These tests exercise
// the back-compat path and exist precisely to pin its behavior until every
// real slot has been migrated. Suppress the deprecation warnings locally so a
// `-Werror` build still compiles them.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace {

class NoEditorSlot : public PluginSlot {
public:
    PluginInfo info_;
    bool bypassed = false;
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
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool b) override { bypassed = b; }
    bool is_bypassed() const override { return bypassed; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
};

// Slot that only overrides the legacy void* API — exercises the default
// create_hosted_editor() compat path.
class LegacySlot : public NoEditorSlot {
public:
    bool created_legacy = false;
    bool destroyed_legacy = false;
    int fake_view = 0;

    bool has_editor() const override { return true; }
    void* create_editor_view() override {
        created_legacy = true;
        return &fake_view;
    }
    void destroy_editor_view() override { destroyed_legacy = true; }
};

class BrokenLegacySlot : public NoEditorSlot {
public:
    bool has_editor() const override { return true; }
    void* create_editor_view() override {
        ++create_calls;
        return nullptr;
    }
    void destroy_editor_view() override { ++destroy_calls; }

    int create_calls = 0;
    int destroy_calls = 0;
};

// Slot that implements the new typed API directly.
class TypedSlot : public NoEditorSlot {
public:
    std::unique_ptr<HostedEditor> create_hosted_editor(void*) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = reinterpret_cast<void*>(0xDEADBEEF);
        ed->width = 640;
        ed->height = 480;
        ed->resizable = true;
        created = true;
        return ed;
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        destroyed = (ed && ed->native_handle != nullptr);
    }
    bool created = false;
    bool destroyed = false;
};

} // namespace

TEST_CASE("Node ABI version is visible through PluginSlot surface",
          "[host][hosted-editor][node-abi]") {
    REQUIRE(pulp::PULP_NODE_ABI_VERSION == 1);
    REQUIRE(pulp::pulp_node_abi_version() == pulp::PULP_NODE_ABI_VERSION);
}

TEST_CASE("no-editor slot returns nullptr from create_hosted_editor",
          "[host][hosted-editor]") {
    NoEditorSlot s;
    auto ed = s.create_hosted_editor(nullptr);
    REQUIRE(ed == nullptr);
}

TEST_CASE("legacy slot is wrapped through the compat path",
          "[host][hosted-editor]") {
    LegacySlot s;
    auto ed = s.create_hosted_editor(nullptr);
    REQUIRE(ed != nullptr);
    REQUIRE(ed->native_handle == &s.fake_view);
    REQUIRE(s.created_legacy);
    s.destroy_hosted_editor(std::move(ed));
    REQUIRE(s.destroyed_legacy);
}

TEST_CASE("legacy compat path handles null editor handles and null destroys",
          "[host][hosted-editor][coverage][phase3]") {
    BrokenLegacySlot broken;
    auto ed = broken.create_hosted_editor(nullptr);
    REQUIRE(ed == nullptr);
    REQUIRE(broken.create_calls == 1);
    REQUIRE(broken.destroy_calls == 0);

    LegacySlot legacy;
    legacy.destroy_hosted_editor(nullptr);
    REQUIRE_FALSE(legacy.destroyed_legacy);
}

TEST_CASE("typed slot populates size + resizable",
          "[host][hosted-editor]") {
    TypedSlot s;
    auto ed = s.create_hosted_editor(nullptr);
    REQUIRE(ed);
    REQUIRE(s.created);
    REQUIRE(ed->width == 640);
    REQUIRE(ed->height == 480);
    REQUIRE(ed->resizable);
    s.destroy_hosted_editor(std::move(ed));
    REQUIRE(s.destroyed);
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
