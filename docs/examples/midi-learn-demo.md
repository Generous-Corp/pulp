# MidiLearnDemo

**Category**: reference
**Type**: Effect
**Path**: `examples/midi-learn-demo/`

## Summary

A stereo level and pan effect whose two parameters can be mapped to hardware controllers. It is the smallest plug-in that uses every part of MIDI Learn.

## What It Demonstrates

- Owning a `state::MidiLearnEngine` in the processor and registering it with `format::MidiLearnHandle`, so a host that reports live controller input (VST3 `IMidiLearn`) can reach it.
- One `format::midi_learn_process_block()` call per audio block: it routes incoming MIDI to the mapped parameters and sends feedback to the controller on the plug-in's MIDI output.
- Reporting feedback support for the format an entry point is built as, through `format::midi_feedback_capabilities()`. Each entry point passes its own format, so the claim matches the build.
- Saving the mappings with the plug-in state (`serialize_plugin_state()` and `deserialize_plugin_state()`), so they survive preset changes and sessions.
- Editor pieces in `midi_learn_demo_ui.hpp`: two knobs bound to the parameters, a right-click MIDI Learn menu on each, an overlay that shows listening and mapped states, Escape to cancel, and undo for every mapping edit.

## How To Use

Right-click a knob and choose **MIDI Learn**, then move a control on the hardware. The knob shows that it is listening, binds when the controller has been still for a moment, and then follows it. **Clear MIDI Mapping** removes the mapping. Both are undoable.

Feedback is sent only when the host exposes the plug-in's MIDI output. The example reports what its adapter writes; whether a host forwards it to a controller depends on the host.

## Tests

`test/test_midi_learn_example.cpp` drives the example headless: the right-click menu starts a learn, a controller binds it, the controller drives the parameter, automation is sent back without echo, undo reverses the learn, Escape cancels, and the mappings round-trip through saved state. Run it with `ctest --test-dir build -R pulp-test-midi-learn-example`.

See [MIDI Learn](../guides/midi-learn.md) for the API.
