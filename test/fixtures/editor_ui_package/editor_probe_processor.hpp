#pragma once

// A processor that does not override editor_factory(). Its descriptor name
// reports which factory editor_factory() returns ("editor:a", "editor:b",
// "editor:none"), so a host reads the plug-in's editor choice through the
// plug-in's own CLAP factory, with no extra exported symbol.

#include "tagged_editor_factory.hpp"

#include <pulp/format/processor.hpp>

#include <memory>
#include <string>

namespace pulp::test_fixtures {

class EditorProbeProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = std::string("editor:") + editor_tag(),
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.test.editor-ui-package",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        output.clear();
    }

  private:
    // editor_factory() is non-const because a factory may build editors; the
    // probe only reads which one it is.
    const char* editor_tag() const {
        auto* factory = const_cast<EditorProbeProcessor*>(this)->editor_factory();
        if (factory == nullptr)
            return "none";
        auto* tagged = dynamic_cast<TaggedEditorFactory*>(factory);
        return tagged != nullptr ? tagged->tag() : "unknown";
    }
};

inline std::unique_ptr<format::Processor> create_editor_probe() {
    return std::make_unique<EditorProbeProcessor>();
}

} // namespace pulp::test_fixtures
