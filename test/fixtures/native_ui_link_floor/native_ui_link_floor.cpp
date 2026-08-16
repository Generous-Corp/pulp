#include <pulp/format/processor.hpp>
#include <pulp/view/editor_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

class NativeScriptRoot final : public pulp::view::View {
public:
    explicit NativeScriptRoot(pulp::state::StateStore& store)
        : session_(*this, store,
                   {.script_path = write_fixture_script(),
                    .enable_hot_reload = false}) {
        std::string error;
        session_.load(&error);
    }

    pulp::view::ScriptedUiSession& session() noexcept { return session_; }

private:
    static std::filesystem::path write_fixture_script() {
        const auto path = std::filesystem::temp_directory_path() /
                          "pulp-native-link-floor-ui.js";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"js(
const root = createView('root', { flexDirection: 'column', padding: 16 });
const title = createLabel('title', 'Native Scripted UI');
appendChild(root, title);
setRoot(root);
)js";
        return path;
    }

    pulp::view::ScriptedUiSession session_;
};

class NativeUiLinkFloorProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor result;
        result.name = "Pulp Native Link Floor";
        result.manufacturer = "Pulp";
        result.bundle_id = "dev.pulp.test.native-link-floor";
        result.version = "1.0.0";
        result.category = pulp::format::PluginCategory::Effect;
        return result;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store_ = &store;
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto channels = std::min(output.num_channels(), input.num_channels());
        const auto samples = std::min(output.num_samples(), input.num_samples());
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::copy_n(input.channel_ptr(channel), samples,
                        output.channel_ptr(channel));
        }
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        auto root = std::make_unique<NativeScriptRoot>(*store_);
        active_root_ = root.get();
        return root;
    }

    pulp::view::ScriptedUiSession* active_scripted_ui() override {
        return active_root_ ? &active_root_->session() : nullptr;
    }

    const pulp::view::ScriptedUiSession* active_scripted_ui() const override {
        return active_root_ ? &active_root_->session() : nullptr;
    }

    void on_view_closed(pulp::view::View&) override { active_root_ = nullptr; }

private:
    pulp::state::StateStore* store_ = nullptr;
    NativeScriptRoot* active_root_ = nullptr;
    // Pull the native EditorBridge object into the link-floor artifact. This
    // catches WebView methods accidentally cohabiting its native object file.
    pulp::view::EditorBridge bridge_;
};

}  // namespace

std::unique_ptr<pulp::format::Processor> make_native_ui_link_floor() {
    return std::make_unique<NativeUiLinkFloorProcessor>();
}
