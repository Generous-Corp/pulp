// Proves a Processor can be authored, constructed, exercised and destroyed
// against pulp::format-core ALONE, with no view layer anywhere in the link.
//
// This is the promise the core/view split exists to make, and nothing else in
// the tree tests it. Almost every other executable reaches pulp::format
// transitively, so a residual undefined symbol in a core-only link would not
// surface here at all -- the first party to hit it would be an out-of-tree
// consumer building "just the DSP" against the SDK, which is exactly the
// consumer this target was carved out for.
//
// What this catches that an nm sweep cannot: nm is filtered by whatever
// pattern the author chose, and a pattern like `pulp::view::` measures
// view-NAMESPACE coupling rather than view coupling. pulp::format::ViewBridge
// is defined in view_bridge.cpp on the view side, so a core object referencing
// it is invisible to that filter and perfectly visible to the linker. This
// executable applies no filter: if anything in format-core needs the view
// half, the link fails.
//
// The CMake target deliberately links pulp::format-core and nothing else. Do
// not add pulp::format or pulp::format-view to it -- that would make the link
// succeed for the wrong reason and quietly retire the proof.
//
// Note what this fixture deliberately does NOT do: it never calls
// Processor::create_view(). That is not an oversight, it is the boundary.
// create_view() returns std::unique_ptr<view::View>, and destroying that
// unique_ptr at the CALL site requires view::View complete -- so a view-free
// consumer cannot call it, even though format-core defines it. Defining and
// calling have different completeness requirements, and only the definition
// is view-free (see format.cpp).
//
// Nothing is lost by not calling it. Deriving from Processor emits this TU's
// own vtable for CoreOnlyProcessor, whose create_view slot points at the
// un-overridden Processor::create_view, so the symbol must still resolve out
// of pulp-format-core at link. That is the property worth proving, and it is
// a link requirement rather than a compile one.

#include <pulp/format/processor.hpp>
#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/state/store.hpp>

#include <cstdio>
#include <memory>

namespace {

class CoreOnlyProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "FormatCoreOnlyConsumer";
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        audio_output.clear();
    }
};

} // namespace

int main() {
    // Constructing and destroying through a base pointer forces the vtable and
    // the virtual destructor chain to resolve, which is the part that would
    // strand if the key function had moved out of format-core.
    std::unique_ptr<pulp::format::Processor> processor =
        std::make_unique<CoreOnlyProcessor>();

    const auto descriptor = processor->descriptor();
    if (descriptor.name != "FormatCoreOnlyConsumer") {
        std::fprintf(stderr, "descriptor did not round-trip\n");
        return 1;
    }

    std::puts("format-core-only consumer linked and ran without the view layer");
    return 0;
}
