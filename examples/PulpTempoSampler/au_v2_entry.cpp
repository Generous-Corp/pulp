// PulpTempoSampler AU v2 entry point.
// Factory function: PulpTempoSamplerAUFactory (matches the generated Info.plist).
//
// PulpTempoSampler is an instrument (descriptor category = Instrument → `aumu`
// Info.plist type), so it MUST register with the MusicDevice factory via
// PULP_AU_INSTRUMENT (PulpAUInstrument + ausdk::AUMusicDeviceFactory). The plain
// PULP_AU_PLUGIN macro registers ausdk::AUBaseFactory, which never wires the
// MusicDevice MIDI selectors — so an `aumu` host's MusicDeviceMIDIEvent call
// returns unimpErr and the AU receives no MIDI (slice taps still play because
// they go through the UI→audio queue, not host MIDI). That mismatch is exactly
// the AU-only "responds to taps but not MIDI" bug.

#include "pulp_tempo_sampler.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(PulpTempoSamplerAU, pulp::examples::create_pulp_tempo_sampler)
