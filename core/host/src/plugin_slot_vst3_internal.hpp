#pragma once

// Internal VST3-slot factory: build a slot around caller-created SDK interfaces
// instead of a bundle on disk.
//
// load_vst3_plugin() dlopens a .vst3, walks GetPluginFactory, and creates the
// component and controller itself. That couples the slot's PluginSlot behavior
// — connection-point wiring, teardown, editor negotiation — to a real plug-in
// on disk, which is the wrong granularity for exercising a call sequence under
// test. In particular the editor path only fully exists once something is
// holding the IPlugFrame: a plug-in may call IPlugFrame::resizeView from
// inside attached(), and no free-function seam can cover that composition.
//
// The slot takes ownership on the same terms as the real loader: it terminates
// and releases the interfaces it is handed. Pass reference-counted fakes that
// tolerate that (or neuter their refcounts). There is no module handle and no
// factory, so nothing is dlclose'd.

#include <pulp/host/plugin_slot.hpp>

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <cstdint>
#include <memory>

namespace pulp::host {

/// Monotonic transport owned by Pulp's in-process VST3 host. A non-null VST3
/// ProcessContext makes projectTimeSamples authoritative, so publishing zero
/// for every block is a seek on every block and forces streaming processors to
/// clear their history continuously.
class Vst3HostProcessClock {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate;
        sample_position_ = 0;
    }

    void write(Steinberg::Vst::ProcessContext& context) const noexcept {
        context.sampleRate = sample_rate_;
        context.projectTimeSamples = sample_position_;
        context.state = Steinberg::Vst::ProcessContext::kPlaying;
    }

    void advance(int num_samples) noexcept {
        if (num_samples > 0)
            sample_position_ += static_cast<std::int64_t>(num_samples);
    }

    double sample_rate() const noexcept { return sample_rate_; }
    std::int64_t sample_position() const noexcept { return sample_position_; }

private:
    double sample_rate_ = 44100.0;
    std::int64_t sample_position_ = 0;
};

/// Build a VST3 slot around already-created interfaces. `controller` may be the
/// same object as `component` (a combined plug-in) or a separate one; the slot
/// resolves which and wires connection points accordingly. Returns nullptr if
/// `component` is null.
std::unique_ptr<PluginSlot> make_vst3_slot(PluginInfo info,
                                           Steinberg::Vst::IComponent* component,
                                           Steinberg::Vst::IAudioProcessor* processor,
                                           Steinberg::Vst::IEditController* controller);

} // namespace pulp::host
