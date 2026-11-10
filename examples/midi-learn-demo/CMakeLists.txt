# MidiLearnDemo: a level and pan effect that shows MIDI Learn end to end
# (engine, learn flow, feedback to the controller, saved mappings, undo).
# The headless test lives in test/test_midi_learn_example.cpp.

pulp_add_plugin(MidiLearnDemo
    FORMATS         VST3 CLAP
    PLUGIN_NAME     "MidiLearnDemo"
    BUNDLE_ID       "com.pulp.midi-learn-demo"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PMLd"
    MANUFACTURER_CODE "Pulp"
)
