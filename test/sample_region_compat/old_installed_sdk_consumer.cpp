// Frozen source consumer authored against the pre-sample-region SDK. This is
// intentionally ordinary user code: later SDKs must continue to compile it.
#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/host/custom_node_type.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/runtime/node_abi.hpp>

#include <iostream>
#include <type_traits>

namespace {
using namespace pulp;

class OldProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "C0 old consumer",
                .manufacturer = "Pulp",
                .bundle_id = "dev.pulp.c0-old",
                .version = "1.0.0",
                .category = format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        for (std::uint32_t c = 0; c < out.num_channels(); ++c)
            for (std::size_t i = 0; i < out.num_samples(); ++i)
                out.channel_ptr(c)[i] = c < in.num_channels() ? in.channel_ptr(c)[i] : 0.0f;
    }
};

// Positional initialization is the source-compatibility seam protected by
// COMP-07. New fields may only remain trailing/defaulted.
host::CustomNodeType old_custom{
    "dev.pulp.compat.old",
    1,
    1,
    1,
    "Old positional node",
    [](audio::BufferView<float>& out, const audio::BufferView<const float>& in, int n) {
        for (int i = 0; i < n; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }};

static_assert(PULP_NODE_V1_ABI_MAJOR == 1u);
static_assert(PULP_NODE_ABI_VERSION == 1u);
static_assert(std::is_same_v<decltype(&pulp_node_v1_abi_major), std::uint32_t (*)()>);
static_assert(std::is_same_v<decltype(&pulp_node_v1_entry), const pulp_node_entry_v1* (*)()>);
static_assert(std::is_same_v<format::reload::ReloadAbiVersionFn, int (*)()>);
static_assert(std::is_same_v<format::reload::ReloadFingerprintFn,
                             void (*)(format::reload::BuildFingerprint*)>);
static_assert(std::is_same_v<format::reload::ReloadCreateFn, format::Processor* (*)()>);
static_assert(std::is_same_v<format::reload::ReloadDestroyFn, void (*)(format::Processor*)>);
} // namespace

int main() {
    OldProcessor processor;
    host::SignalGraph graph;
    const bool valid = processor.descriptor().category == format::PluginCategory::Effect &&
                       old_custom.version == 1 && pulp_node_v1_abi_major() == 1u &&
                       graph.nodes().empty();
    if (valid)
        std::cout << "old-installed-sdk-consumer=pass\n";
    return valid ? 0 : 1;
}
