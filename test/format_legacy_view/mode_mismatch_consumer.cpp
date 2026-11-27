// A plug-in compiled with PULP_FORMAT_LEGACY_VIEW=0, linked against a
// pulp-format-core built with Pulp's view layer. The link must fail on the
// marker that names the mismatch (processor.hpp), not on a reserved slot.
#include <pulp/format/processor.hpp>

namespace {
class Quiet final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};
} // namespace

int main() {
    Quiet processor;
    return processor.has_editor() ? 0 : 1;
}
