---
name: midi-learn
description: Add or change MIDI Learn in a Pulp plug-in or in the SDK itself. The engine, learn flow, saved mappings, undo, controller feedback, the VST3 IMidiLearn hook, the view menu and overlay, plus the test and landing traps that cost time.
---

# MIDI Learn

Use this skill when a plug-in should let the user map hardware controllers to
parameters, or when you change the MIDI Learn code in `core/state`,
`core/format` or `core/view`. The user guide is `docs/guides/midi-learn.md`; the
worked example is `examples/midi-learn-demo/`.

## The pieces

- `state::MidiLearnEngine` (`core/state/.../midi_learn_engine.hpp`): mappings,
  routing, the learn flow, feedback. UI-thread calls build an immutable table
  and publish it through a triple buffer; the audio thread adopts it in
  `begin_block()`. Call `route()` and `emit_feedback()` only on the audio thread.
- Pure functions you can call anywhere: `decode_simple`, `Cc14Decoder`,
  `NrpnDecoder`, `decode_relative`, `classify_learn_sequence`, `takeover_step`,
  `encode_feedback`. Test against these before touching audio.
- `state::MidiLearnEditor` records edits as `UndoManager` steps by whole-table
  snapshots. `replace_all()` keeps mapping `uid`s, which is what lets takeover
  state and undo coexist.
- Persistence: `save_state()` / `load_state()` (versioned binary chunk) and
  `save_json()` / `load_json()`. Mappings are global by default; a processor that
  wants per-preset mappings stores the chunk in its preset data.
- `format::midi_learn_process_block()` is the one per-block call;
  `format::MidiLearnHandle` registers the engine so the VST3 adapter can offer
  `IMidiLearn`. Declare the handle after the engine member.
- `view::MidiLearnTarget` is a plain callback struct; the menu rows, attach
  helper and overlay live in `midi_learn_menu.hpp`, and `MidiLearnSession` adapts
  them to the engine.

## Invariants worth keeping

- One learned source drives one parameter, and a learned parameter has one
  source. A new binding removes overlapping mappings (same controller on an
  overlapping channel, or the same parameter) and returns them for undo.
- The learn queue has a single producer (the audio thread) and a single consumer
  (`poll_learn()`). Anything that can arrive from another thread, such as a host's
  live-controller report, goes through `host_reported_controller()`, an atomic
  slot, never the queue.
- A plug-in that uses some MIDI messages itself (a preset router consuming Bank
  Select and Program Change) claims them with a `state::MidiSourceReservation` and
  `engine.set_reservation()`. Claimed sources cannot be mapped or learned (the
  outcome carries `refused_reason`), are dropped on load, and are skipped by
  `route()` even for an older saved mapping. `claims()` runs on the audio thread:
  no allocation or locks, and an omni source overlaps any channel the owner uses.
- `MidiLearnSession` shows a refusal reason for `kRefusalShowMs`, but a learn that
  binds clears it at once, so a retry that succeeds never shows the old reason.
- Feedback never echoes a value the controller just wrote, and the first call
  after a mapping appears sends the current value so the controller can sync.
- `midi_feedback_capabilities()` says what the adapter writes to the host's MIDI
  output, nothing about what a host does with it. Add a format only after reading
  its outbound path. Do not make claims about a host you have not observed.

- Mapping settings in the right-click menu (invert, relative mode, takeover,
  feedback) go through `MidiLearnTarget::set_options`. Each control sends the
  full settings with its own field changed, and `MidiLearnSession` applies them
  through the undo editor, which records a step only when the mapping really
  changed. The menu's choice lists are in enum order, so adding an enum value
  means adding its label. Invert swaps the mapping's window ends; do not store a
  separate flag.

## Testing traps

- Suites registered with `GROUP` have no target of their own. Build the group
  (for example `pulp-test-group-cap-state`) and select a suite with a tag filter.
- With `confirm_failure.sh`, a header-only edit needs `--object <test source
  stem>`; a `.cpp` edit must not pass `--object` (the default is the source's own
  object, and the wrong name reads as INCONCLUSIVE). A break pattern must match
  the file as clang-format left it, so check the real text first.
- Vellum routing: new headers under `core/state`, `core/format` or `core/view`
  need a `vellum_inherits` decision in `.github/vellum-export-events/`, committed
  before the check can see it. Anything under `core/view` also needs a watch
  event.

## Landing traps

- New public headers must be listed in `docs/status/consumption-profiles.json`
  (one list per include root). `test_consumption_census.py` runs locally and
  catches it. A new standalone test executable needs
  `test/ctest_script_inputs/executables/<name>.json`.
- A new test translation unit grows the fan-out of the headers it includes; the
  pre-push guard names them and wants `Fanout-Grow:` trailers.
- Editing `core/format/include/pulp/format/processor.hpp` changes a fingerprint
  pinned in `tools/scripts/agent_capability_catalog_performance.py`; see the
  `agent-capabilities` skill. The engine registration uses a side table so
  `sizeof(Processor)` and the vtable do not change.
- A `docs/reference/widgets.md` row is required for every developer-facing View
  primitive (`check-docs`).

## Forge reachability

MIDI Learn is a plug-in-instance feature: it maps a controller to a parameter of
the processor that owns the engine. It adds no audio, MIDI-processing or
timeline node, so it has no Forge catalog entry. The owner of that decision is
the processor's author; a Forge app that hosts a Pulp processor gets MIDI Learn
by using `MidiLearnEngine` in that processor.
