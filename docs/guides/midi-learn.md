# MIDI Learn

`pulp::state::MidiLearnEngine` (`pulp/state/midi_learn_engine.hpp`) lets a
plug-in map hardware controllers to parameters, learn a mapping by moving a
control, and apply incoming MIDI to the parameters in real time. It supersedes
`MidiParameterMap`, which remains for existing callers.

## What it maps

| Source (`MidiSourceKind`) | Notes |
|---|---|
| `cc7` | 7-bit CC. Absolute, or a relative encoder (see below). |
| `cc14` | 14-bit CC pair, MSB 0..31 with LSB at +32. An MSB with no LSB decodes at 7 bits, so MSB-only controllers still reach full range. |
| `nrpn`, `rpn` | Parameter selection (CC 99/98, 101/100) plus data entry (CC 6, optional 38). Increment/decrement (CC 96/97) move the parameter by ticks. |
| `pitch_bend`, `channel_pressure`, `poly_pressure`, `program_change` | Poly pressure is keyed by note. |

Each `MidiMapping` carries a normalized window (`min > max` inverts), a relative
mode, a tick size and acceleration, and a takeover mode (`jump`, `pickup`,
`scale`). Channels are fixed (0..15) or omni.

Relative encoders are `twos_complement` (1..63 up, 127..65 down), `signed_bit`
(bit 6 is the sign) and `binary_offset` (64 is zero).

## Wiring it into a processor

```cpp
// process(), on the audio thread:
learn_.begin_block();
for (const auto& e : midi_in)
    learn_.route(state(), pulp::state::raw_midi_from_bytes(e.data(), e.size()));
```

`route()` allocates nothing and takes no locks. UI edits (`set_mapping`,
`clear`, `clear_source`, `clear_all`) build a new immutable table and publish it
through a triple buffer; the audio thread adopts it at the next `begin_block()`.
Takeover state follows a mapping across edits by its `uid`.

## Learning

```cpp
learn_.begin_learn(param_id, now_ms);     // UI thread
// from a UI timer:
if (auto out = learn_.poll_learn(now_ms)) {
    // out->mapping is the new binding; out->replaced lists what it displaced
}
learn_.cancel_learn();                    // Escape / click away
```

While listening, the audio thread copies learnable messages into a queue.
`poll_learn()` gathers them into a burst and, once the controller has been quiet
for `settle_ms` (default 250), classifies the burst with
`classify_learn_sequence()` and binds it. The classifier recognises 14-bit pairs,
NRPN/RPN and relative encoders; relative detection needs at least three events
and reports a confidence, so a UI can offer a mode override when it is low.

One source drives one parameter, and a learned parameter has one source. A new
binding removes any mapping for the same controller (overlapping channels count)
or the same parameter and returns them in `replaced`.
`learn_state()` reports `idle`, `listening` or `bound` for the UI, and a listen
gives up after `timeout_ms`.

## Saving and loading

Mappings belong to the controller, not the sound, so by default they are saved
once per plug-in instance (global) and survive preset changes. A processor that
wants per-preset mappings stores the chunk in its preset data instead; the
engine does not decide.

```cpp
// serialize_plugin_state(): append the chunk to the processor's blob
auto chunk = learn_.save_state();                    // versioned binary chunk
// deserialize_plugin_state(): hand the chunk back (an empty span clears)
auto report = learn_.load_state(chunk, &state());    // drops unknown parameters
```

`load_state()` reports `status`, how many mappings loaded and how many were
dropped (invalid, over capacity, or naming a parameter the store does not have).
A chunk written before mappings existed is empty and loads as no mappings. A
chunk from a newer version loads its known fields and reports `newer_version`.
Damaged data (bad magic, size or checksum) is rejected and the current table is
kept. The wire layout is documented in `midi_learn_persistence.hpp` and pinned by
a test.

`save_json()` / `load_json()` produce and read a text form for preset and
import/export files (`{"version":1,"mappings":[...]}`). Import takes a
`MidiImportMode`: `replace` makes the file the whole table, `merge` lets the
file's mappings displace overlapping ones and keeps the rest.

## Undo

`MidiLearnEditor` (`midi_learn_undo.hpp`) wraps the engine and an `UndoManager`;
each edit is one undo step:

```cpp
MidiLearnEditor editor(learn_, undo_);
editor.set_mapping(mapping);
editor.clear(param_id);
editor.record_learn(*outcome);   // a learn the engine already applied
```

Undo of a learn removes the new mapping and restores whatever it replaced.
Mappings keep their `uid` across undo, so takeover state is not reset. The engine
must outlive the undo history.

## Pure pieces

`midi_decode.hpp` and `midi_learn.hpp` hold the decoders, `decode_relative`,
`apply_relative`, `classify_learn_sequence` and `takeover_step` as pure functions
over raw bytes, so a widget, a test or another subsystem can use them without
running audio.
