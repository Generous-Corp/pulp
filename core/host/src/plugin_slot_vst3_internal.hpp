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

#include <memory>

namespace pulp::host {

/// Build a VST3 slot around already-created interfaces. `controller` may be the
/// same object as `component` (a combined plug-in) or a separate one; the slot
/// resolves which and wires connection points accordingly. Returns nullptr if
/// `component` is null.
std::unique_ptr<PluginSlot> make_vst3_slot(PluginInfo info,
                                           Steinberg::Vst::IComponent* component,
                                           Steinberg::Vst::IAudioProcessor* processor,
                                           Steinberg::Vst::IEditController* controller);

} // namespace pulp::host
