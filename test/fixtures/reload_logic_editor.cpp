// Hot-reload logic fixture with an editor, for test_reloadable_editor_shell.cpp.
//
// Built once per PULP_RELOAD_FIXTURE_GEN (and, for the mode-mismatch case, at
// the other PULP_FORMAT_LEGACY_VIEW). Same parameter contract as the other
// reload fixtures (one "Gain" param, id 1, range 0..2), so a swap between
// generations is accepted. Its editor-host factory lives in this image; each
// editor it builds reports what happens to it to the host through
// EditorContext::host_action(mark_state_dirty) with
// param = generation * 100 + event, so the host sees which image a call came
// from without either side naming the other's types.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>

#ifndef PULP_RELOAD_FIXTURE_GEN
#error "PULP_RELOAD_FIXTURE_GEN must be defined"
#endif

using namespace pulp;

namespace {

enum Event : int { built = 1, attached = 2, detached = 3, destroyed = 4, ticked = 5, resized = 6 };

class ReportingEditor final : public format::Editor {
  public:
    explicit ReportingEditor(format::EditorContext& context) : context_(context) {
        report(built);
    }
    ~ReportingEditor() override {
        report(destroyed);
    }
    bool supports_parent(format::NativeParentKind) const override {
        return true;
    }
    bool attach(const format::NativeParent&) override {
        report(attached);
        return true;
    }
    void detach() override {
        report(detached);
    }
    void invalidate() override {}
    format::EditorSize size() const override {
        return size_;
    }
    bool set_size(const format::EditorSize& size) override {
        size_ = size;
        report(resized);
        return true;
    }
    void idle() override {
        report(ticked);
    }

  private:
    void report(Event event) {
        format::HostActionRequest request;
        request.action = format::HostAction::mark_state_dirty;
        request.param = static_cast<state::ParamID>(PULP_RELOAD_FIXTURE_GEN * 100 + event);
        context_.host_action(request);
    }

    format::EditorContext& context_;
    format::EditorSize size_{300 + PULP_RELOAD_FIXTURE_GEN, 200, 1.0};
};

class ReportingFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        return supports(role) ? std::make_unique<ReportingEditor>(context) : nullptr;
    }
};

class EditorGain final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain",
                .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain",
                .version = "0.1.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "", .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n)
                o[n] = i[n] * static_cast<float>(PULP_RELOAD_FIXTURE_GEN);
        }
    }
    format::EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    ReportingFactory factory_;
};

} // namespace

PULP_RELOAD_LOGIC(new EditorGain())
