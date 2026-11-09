# An AU MIDI effect that answers every note an octave up; the AUAudioUnit
# slot's MIDI output test hosts it. See host_midi_probe.hpp.
if(NOT APPLE OR PULP_IOS OR NOT PULP_HAS_AUSDK)
    return()
endif()

pulp_add_plugin(PulpHostMidiProbe
    NO_EDITOR
    FORMATS           AU
    PLUGIN_NAME       "Pulp Host MIDI Probe"
    BUNDLE_ID         "dev.pulp.test.host-midi-probe"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          MidiEffect
    PLUGIN_CODE       "PHmp"
    MANUFACTURER_CODE "PlHp"
    SOURCES           host_midi_probe.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_host_midi_probe
)
