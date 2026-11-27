// Asks a hot-reload shell, linked the way its plug-in is, for an editor: it
// loads the logic, then checks supports(editor) and create() without attaching,
// so no window opens. Built by cmake-vellum-ui-in-tree-real in the in-tree mode.
#include PROBE_HEADER

#include <pulp/format/editor_host.hpp>
#include <pulp/format/reload/reloadable_editor_shell.hpp>
#include <pulp/state/store.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::state::StateStore& store) : port_(store) {}
    pulp::format::ParamPort& params() override { return port_; }
    pulp::format::VisualPort* visuals() override { return nullptr; }
    bool request_resize(const pulp::format::EditorSize&) override { return true; }
    bool host_action(const pulp::format::HostActionRequest&) override { return true; }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::StateStoreParamPort port_;
};
} // namespace

int main() {
    setenv("PULP_RELOAD_LOGIC_PATH", PROBE_LOGIC, 1);
    auto shell = PROBE_FACTORY();
    auto* control = pulp::format::reload::reloadable_editor_shell_control(*shell);
    pulp::state::StateStore store;
    shell->define_parameters(store);
    shell->set_state_store(&store);
    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 64;
    shell->prepare(prepare);
    const bool active = control != nullptr && control->has_active_dsp();
    auto* factory = shell->editor_factory();
    const bool supports = factory != nullptr && factory->supports(pulp::format::EditorRole::editor);
    Context context(store);
    auto editor = supports ? factory->create(*shell, context, pulp::format::EditorRole::editor)
                           : nullptr;
    std::printf("reload-editor-probe %s: shell=%d logic=%d supports=%d created=%d\n", PROBE_NAME,
                control != nullptr, active, supports, editor != nullptr);
    return control != nullptr && active && supports && editor != nullptr ? 0 : 1;
}
