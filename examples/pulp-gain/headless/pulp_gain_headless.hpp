#pragma once

// PulpGainHeadless: PulpGain built without an editor (pulp_add_plugin
// NO_EDITOR). The DSP and parameters are PulpGain's; only the identity
// differs, so both plug-ins can be installed side by side. Hosts show their
// own generic parameter UI for it.

#include "../pulp_gain.hpp"

namespace pulp::examples {

class PulpGainHeadlessProcessor : public PulpGainProcessor {
  public:
    format::PluginDescriptor descriptor() const override {
        auto d = PulpGainProcessor::descriptor();
        d.name = "PulpGainHeadless";
        d.bundle_id = "com.pulp.gain-headless";
        return d;
    }
};

inline std::unique_ptr<format::Processor> create_pulp_gain_headless() {
    return std::make_unique<PulpGainHeadlessProcessor>();
}

} // namespace pulp::examples
