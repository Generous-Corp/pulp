# MIDI FX

Pulp provides the host-facing MIDI-effect path; Forge provides the generated
ordered transform chain that runs inside it. A MIDI effect receives timestamped
MIDI, transforms or generates events, and produces MIDI. It has no audio DSP or
voice graph.

Use this guide for three things:

- authoring a `.pulpgraph` MIDI chain;
- choosing and combining Forge's MIDI transforms;
- embedding or extending the C++ runtime without violating its real-time and
  note-lifecycle contracts.

## Runtime model

A chain is a linear list of one to eight transforms. Node array order is
processing order; MIDI chains do not use graph connections.

```json
{
  "format_version": 1,
  "name": "Held Human Arp",
  "description": "Latches a chord, arpeggiates it, then loosens the result.",
  "nodes": [
    { "id": 1, "type": "latch", "name": "Latch",
      "params": { "mode": 1, "seed": 1 } },
    { "id": 2, "type": "arp", "name": "Arp",
      "params": { "rate": 1, "mode": 2, "octaves": 2, "hold": 0, "gate": 0.65 } },
    { "id": 3, "type": "humanize", "name": "Feel",
      "params": { "timing_ms": 5, "vel_jitter": 7, "seed": 19 } }
  ],
  "connections": []
}
```

The runtime separates immutable `forge::ChainSpec` data from mutable,
audio-thread-owned `forge::ChainState`. Specs are prepared off the audio thread
and atomically published. A publication change flushes sounding notes before
state is reset.

The hard invariant is no stuck notes: every note-on introduced, transformed, or
delayed by a node is eventually released. A downstream node may consume a voice
that an upstream node still tracks, so a later redundant note-off is valid and
inert; a positive sounding-note balance is not. A chain that fails Forge's MIDI
realtime probe is rejected rather than installed.

## Shared values

Root indices are `0=C` through `11=B`. Scale indices are `major`, `minor`,
`dorian`, `phrygian`, `lydian`, `mixolydian`, `harmonic minor`,
`pentatonic major`, `pentatonic minor`, and `chromatic`.

The shared division indices are:

| Index | Division | Index | Division | Index | Division |
|---:|---|---:|---|---:|---|
| 0 | 1/32 | 5 | 1/16T | 10 | 1/32T |
| 1 | 1/16 | 6 | 1/8T | 11 | 1/32. |
| 2 | 1/8 | 7 | 1/8. | 12 | 1/16. |
| 3 | 1/4 | 8 | 1/4. | 13 | 1/2. |
| 4 | 1/2 | 9 | 1/64 | 14 | 1/1 |

`morph_seq.rate` uses `0=per-lane`; values `1..15` select shared division
indices `0..14`.

## Transform API

These are all public transform types and all of their node-local parameters.
Ranges are canonical: generated ranges are ignored and the loader applies
these definitions.

| Transform | Parameters | Behavior and values |
|---|---|---|
| `transpose` | `semitones` | Shifts notes by `-24..24` semitones. |
| `velocity_map` | `mode`, `amount`, `fixed_vel` | `mode`: `scale`, `compress`, `fixed`; `amount -1..1`; `fixed_vel 1..127`. |
| `scale_lock` | `root`, `scale`, `strength` | Quantizes toward a root and scale; `strength 0..1`. |
| `humanize` | `timing_ms`, `vel_jitter`, `seed` | Deterministic timing `0..30 ms` and velocity `0..40` jitter. Seed is structural, not macro-exposable. |
| `chord` | `type`, `voicing`, `spread` | Types: `maj`, `min`, `dim`, `aug`, `sus2`, `sus4`, `maj7`, `min7`, `dom7`, `power`, `octave`; voicing: `close`, `open`, `drop2`; spread `0..1`. |
| `harmonize` | `interval`, `root`, `scale`, `mix` | Adds one diatonic voice. Interval indices `1..7` mean second through octave; mix `0..1`. |
| `note_delay` | `sync`, `division`, `time_ms`, `feedback`, `vel_decay`, `repeats` | `sync`: `time` or `sync`; time `1..1000 ms`; feedback `0..0.9`; decay `0..1`; repeats `1..16`. |
| `arp` | `rate`, `mode`, `octaves`, `hold`, `gate` | Modes: `up`, `down`, `up-down`, `random`, `as-played`; octaves `1..4`; hold off/on; gate `0.1..1`. |
| `note_repeat` | `sync`, `division`, `count`, `time_ms`, `vel_curve`, `gate` | Retriggers into `1..16` hits; free time `5..500 ms`; velocity curve `-1..1`; gate `0.1..1`. Count 1 is bypass. |
| `chance` | `mode`, `pulses`, `steps`, `seed`, `probability` | Probability or Euclidean gating; pulses `1..16`, steps `1..32`, probability `0..1`. Seed is not macro-exposable. |
| `lfo_cc` | `cc`, `waveform`, `period`, `depth`, `offset` | Emits CC `0..127`; waveform: sine, triangle, saw, square; periods: 2 bars, 1 bar, 1/2, 1/4, 1/8; depth and offset `0..1`. Depth 0 emits nothing. |
| `pattern_gate` | `gate`, `swing`, `fill`, `division`, `mode`, `seed` | Chops held notes from pattern lane 0. Gate `0.05..1`; swing/fill `0..1`; mode pass/chop. Pattern required. |
| `step_seq` | `gate`, `swing`, `humanize`, `fill`, `rate`, `input_mode`, `restart`, `seed` | Multi-lane generator. Input: mute, merge, transpose; restart: free, bar, note; continuous controls `0..1` except gate `0.05..1`. Pattern required. |
| `chord_map` | `vel_scale`, `learn`, `spread`, `mode`, `voicing`, `match`, `unmatched` | Modes: single, multi, degree; voicing: as-stored/nearest; match: exact/pitch-class; unmatched: thru/mute. Velocity scale `0.25..1.5`; learn arms at `>=0.5`; spread `0..1`. Chords required. |
| `strum` | `time`, `shape`, `tilt`, `humanize`, `order`, `sync`, `division`, `seed` | Order: down, up, alternate, random, as-played; sync: ms/synced; time and humanize `0..1`; shape and tilt `-1..1`. Put after a chord source. |
| `note_gen` | `density`, `variation`, `contour`, `range`, `mode`, `follow`, `order`, `seed` | Monophonic melody generator. Modes: pool, Markov, walk, register, dice; follow off/root; Markov order `1..2`; contour `-1..1`; other continuous values `0..1`. Pattern and key-bearing chords required. |
| `counter_gen` | `imperfect_bias`, `range`, `activity`, `reserved`, `position`, `species`, `root_scale_source`, `seed` | Deterministic first-species counterpoint. Position above/below; source chords-block/C major; bias/range `0..1`. `activity` and `reserved` are fixed at 0; structural controls and seed are not macro-exposable. |
| `latch` | `mode`, `seed` | Off, hold, or toggle. CC64 passes through and is not emulated. Seed is reserved and not macro-exposable. |
| `morph_seq` | `morph`, `swing`, `humanize`, `rate`, `seed` | Interpolates `pattern` A and `pattern_b` B. Continuous controls `0..1`; rate `0` uses each lane's division and `1..15` select shared division indices `0..14`. |
| `drum_gen` | `x`, `y`, `density`, `fill`, `rate`, `engine`, `character`, `seed` | Concept or cellular-automata engine. `x`, `y`, density, fill `0..1`; character/rule `0..255`; authored steps remain protected. Pattern with drum roles required. |

Only rows marked macro-exposable in
`forge/gen/midi_transform_catalog.hpp` may be bound to live host macros. Seeds,
structural modes, and reserved controls stay fixed so automation cannot rebuild
prepared models on the audio callback.

## Pattern data

`pattern_gate`, `step_seq`, `note_gen`, `morph_seq`, and `drum_gen` consume
bounded pattern data: at most eight lanes, each with at most 32 steps.

```json
{
  "pattern": {
    "groove": {
      "timing": [0, 8, 2, 10],
      "velocity": [100, 72, 88, 64]
    },
    "lanes": [{
      "note": 36,
      "channel": 9,
      "length": 16,
      "division": "1/16",
      "direction": "forward",
      "choke_group": 0,
      "acc_reset": "pattern_loop",
      "steps": [
        { "on": 1, "vel": 110, "gate": 80 },
        {},
        { "on": 1, "vel": 72, "prob": 60,
          "cond": "a_b", "cond_a": 3, "cond_b": 4 }
      ]
    }]
  }
}
```

Lane fields:

| Field | Values |
|---|---|
| `note`, `channel` | MIDI note `0..127`; zero-based channel `0..15`. |
| `length` | Loop window `1..32`. Different lane lengths create polymeter. |
| `division` | Shared division name/index, or omitted to inherit the node rate. |
| `direction` | `forward`, `reverse`, `pingpong`, `drunk`, `random`. |
| `choke_group` | `0` disables; `1..8` makes lanes in the group cut each other off in chronological order. |
| `acc_reset` | `pattern_loop` (default), `never`, or `manual`. |
| `role`, `concept` | `drum_gen` role and optional vocabulary concept. |

Step fields are `on 0|1`, `vel 1..127`, `ratchet 1..8`, `micro -50..50`
percent, `pitch -24..24`, `prob 0..100`, `gate 5..100` percent, and
`acc_add -24..24`. Conditions are `always`, `prob`, `a_b`, `first`,
`not_first`, `fill`, `not_fill`, `prev`, and `not_prev`; `cond_a` and `cond_b`
are `1..8`.

Groove timing entries are `-50..50` percent of a step. Groove velocity entries
are `25..200` percent. Arrays contain at most 16 entries and repeat.

`morph_seq` also accepts `pattern_b` with the same schema. Its exact endpoints
play A and B; intermediate values interpolate compatible lanes and steps.

## Chord and melody data

`chord_map`, `note_gen`, and `counter_gen` use an attached `chords` block:

```json
{
  "chords": {
    "root": 0,
    "scale": "minor",
    "slots": [
      { "trigger": 60, "notes": [0, 3, 7, 10] },
      { "trigger": 62, "notes": [0, 4, 7, 11], "oct": -1 }
    ]
  }
}
```

`trigger` is a MIDI note number. `notes` contains up to eight semitone offsets
relative to the trigger. `oct` shifts the whole shape by `-2..2` octaves.
There are at most 24 slots in one chord map and at most two maps in a chain.

Live learn in `chord_map` is volatile. While `learn >= 0.5`, teaching notes are
consumed. Single mode stores the released shape immediately. Multi mode captures
the shape, then assigns it to the next trigger note. Learned shadows override
authored slots until reset or hot-swap.

For `note_gen`, lane 0 supplies rhythm and seed pitches. Markov order is prepared
off the audio thread. Dice mode treats lanes 0 through 3 as complete one-bar
fragments and switches only on bar boundaries. A `chords` object with no slots
is valid when only root and scale are needed.

## Drum concepts

`drum_gen` roles are `kick`, `snare`, `clap`, `hat_closed`, `hat_open`, `ride`,
`tom`, `perc`, `crash`, and `generic`. A lane may name a stable concept or omit
it so `x` and `y` select from the style map. Closed hats automatically choke open
hats; explicit nonzero choke groups work across all roles.

Useful concept starting points:

- house: `kick.four_floor`, `snare.backbeat`, `hat_closed.offbeat_8`,
  `hat_open.offbeat`;
- trap: `kick.trap_808`, `snare.half_time_bb`, `hat_closed.trap_roll`;
- boom bap: `kick.boom_bap`, `snare.ghost_cloud`, `hat_closed.shuffle`;
- drum and bass: `kick.dnb_two`, `snare.dnb`, `hat_closed.straight_16`;
- garage: `kick.broken`, `snare.backbeat`, `hat_closed.garage_2step`;
- Afro-Cuban layer: `snare.clave_son`, `perc.bell_e712`,
  `perc.tumbao_generic`.

Concept-engine density is monotonic: zero is silent and increasing density only
adds hits, from anchors through ornaments. Automata mode advances one cellular
generation per bar; `character` is its rule number and 110 is a useful start.

## Macro map

Forge MIDI FX expose a stable bank of 16 host parameters. A generated build
retargets those fixed slots; it never changes the host's automatable parameter
list.

```json
{
  "params": [
    { "id": "param_1", "name": "Rate",
      "node": 2, "node_param": "rate", "default": 1 },
    { "id": "param_2", "name": "Octaves",
      "node": 2, "node_param": "octaves", "default": 2 }
  ]
}
```

IDs must be contiguous from `param_1`, each node and parameter must exist, and
there may be at most 16 entries. The author chooses label and default. The
catalog owns range, curve, unit, stepped labels, and macro eligibility.

## Cookbook

Chain order changes the music:

| Goal | Chain | Why this order |
|---|---|---|
| One-finger guitar | `chord_map → strum → humanize` | Build voices first, schedule the rake second, loosen final events once. |
| Performable held arp | `latch → scale_lock → arp` | Latch physical input, tune the held set, then clock it. |
| Evolving but bounded melody | `note_gen → scale_lock → chance` | Generate from prepared seed material, enforce pitch, then thin density. |
| Polymetric chords | `step_seq → chord → note_delay` | Generate roots, expand them, then echo complete voicings. |
| Drum performance | `drum_gen → humanize` | Keep density/fill structural and add a light final feel pass. |
| First-species study | `scale_lock → counter_gen` | Normalize the cantus before deriving its consonant voice. |

For stopped DAW transport, set `ChainState::allow_free_run` to `false`. Use
`true` only for editor preview. Before replacing a live chain, reserve
`max_flush_events()` in the flush buffer, call `flush_all_notes_off()`, and only
then reset or discard the old state.

## C++ API reference

The Forge headers are the source of truth. This table documents every public
method on the MIDI-chain runtime and loader surface.

### Chain runtime (`forge/midi_transform.hpp`)

| Method | Contract |
|---|---|
| `midi_transform_kind_name(kind)` | Returns the canonical node type name for a `MidiTransformKind`. |
| `midi_transform_kind_from_name(name, out)` | Parses a canonical name or supported alias into `out`; returns `false` without accepting an unknown node. |
| `ChainState::reset()` | Clears bounded per-transform state. Call on the control thread after sounding notes have been flushed. It preserves no held-note or scheduling state. |
| `prepare_chain_spec(spec)` | Builds bounded Markov and cellular-automata lookup/checkpoint data after load or edit. Control-thread only; call before immutable publication. |
| `run_chain(ChainSpec&, state, in, out, context, scratch_a, scratch_b)` | Compatibility/control-thread overload. Prepares a mutable legacy spec once if needed, then dispatches to the realtime overload. Do not rely on it to mutate specs in an audio callback. |
| `run_chain(const ChainSpec&, state, in, out, context, scratch_a, scratch_b)` | Realtime overload for a prepared immutable spec. Clears `out`, composes nodes through pre-reserved ping-pong buffers, mutates only `state`, performs bounded work, and allocates nothing. |
| `max_flush_events()` | Returns the worst-case number of note-offs a complete chain flush may emit. Reserve at least this capacity before realtime use. |
| `flush_all_notes_off(spec, state, out)` | Emits releases for every sounding note and clears the relevant state. Used at hot-swap and teardown; `out` must already have realtime-safe capacity. |
| `reset_step_accumulators(state, transform_index, lane_mask=0xFF)` | Requests a deterministic manual reset for selected lanes. The audio callback consumes each bit immediately before the lane's next tick. |

`MidiTransformKind`, `TransformSpec`, `PatternBlock`, `ChordMapBlock`,
`ChainSpec`, and `ChainState` are bounded, trivially-copyable data contracts.
Do not add heap-owning fields or bypass the documented capacities.

### Catalog and loader

| Method | Contract |
|---|---|
| `gen::midi_param_row(kind, name)` | Returns the canonical slot/range/display row, or `nullptr` for an invalid transform-parameter pair. |
| `gen::midi_param_labels(row)` | Copies a stepped row's canonical labels for host or UI presentation. Returns an empty vector for continuous rows. Control-thread use. |
| `gen::load_midi_chain(bundle, max_macros)` | Parses, validates, lowers, reserves structured-data capacity, resolves macros against the catalog, prepares the spec, and returns a `MidiChainLoadResult`. On failure `ok=false`, `stage` and `message` identify the rejection, and the partial chain must not be installed. |
| `gen::midi_chain_load_stage_name(stage)` | Returns a stable human-readable name for loader diagnostics. |
| `gen::midi_transform_system_context()` | Returns the generator's authoritative transform/schema context. It is immutable process-lifetime data. |

`MidiChainLoadResult::warnings` are non-fatal normalization or capacity notices.
The stages are `None`, `ParseChain`, `UnsupportedTransform`, `Capacity`,
`ResolveMacro`, and `Loaded`.

### Macro map (`forge/param_map.hpp`)

| Method | Contract |
|---|---|
| `format_param_value(display, unit, plain)` | Formats a canonical plain value for the host readout, including stepped, percent, swing, time, frequency, multiplier, and bit displays. |
| `MacroBinding::apply(normalized)` | Maps a normalized host value to the binding's canonical plain range. |
| `MacroBinding::invert(plain)` | Maps a canonical plain value back to normalized host state. |
| `ParamMap::from_json(json, out, err)` | Parses JSON into `out`; returns `false` and a typed diagnostic on failure. Extra fields are tolerated, then schema rules are enforced by `validate()`. |
| `ParamMap::from_file(path, out, err)` | Reads and parses a macro-map file with the same result contract. Control-thread only. |
| `ParamMap::validate(err)` | Rejects over-capacity maps, invalid or duplicate slots, missing names/targets, and unknown curves. |
| `ParamMap::for_slot(slot)` | Returns the binding for a host slot or `nullptr` if unbound. |

### Shell (`forge/midi_shell.hpp`)

| Method | Contract |
|---|---|
| `ForgeMidiShell()` | Installs a safe default transpose chain and fixed host macro surface. |
| `descriptor()` | Declares a MIDI-effect processor with the format metadata used by AU/CLAP adapters. |
| `define_parameters(store)` | Declares the fixed 16-slot host parameter bank once. |
| `prepare(context)` | Records sample rate/block size and reserves every realtime MIDI buffer. |
| `process_audio(audio_out, audio_in, midi_in, midi_out, context)` | Ignores audio content, applies current macro values, flushes on generation changes, and runs the immutable chain over MIDI. Realtime-safe after `prepare()`. |
| `install_generated_chain(spec, macros, err)` | Validates macro budget, prepares and publishes the spec plus bindings atomically, and seeds host defaults. On failure leaves the running build untouched. |
| `has_build()` | Always `true`; a default chain exists before generation. |
| `macro_descriptors()` | Returns current host-facing labels, ranges, defaults, and displays for bound macro slots. |
| `install_generated_bundle(bundle, sample_rate, block_size, progress, info, err)` | Lowers and verifies a generated bundle before installing it. Reports progress and rejection details; never publishes a failed candidate. |
| `reset_to_default_build()` | Replaces the current generated chain with the default transpose build. |
| `ensure_default_build()` | Ensures the default exists without needlessly replacing an installed chain. |
| `restore_macro(slot, normalized)` | Restores one persisted normalized macro value through the current binding. |
| `apply_macro_from_store(slot, normalized)` | Applies one store-originated normalized value to the fixed host slot. |
| `current_macro_positions()` | Returns the currently bound slot/value pairs for persistence. |
| `current_sample_rate()` | Returns the most recently prepared sample rate. |
| `current_block_size()` | Returns the most recently prepared maximum block size. |
| `create_forge_midi()` | Creates the `pulp::format::Processor` used by MIDI-capable format adapters. |

## Host and validation notes

On macOS the AU component type is `aumi` (`kAudioUnitType_MIDIProcessor`).
The host renders its silent output element to advance MIDI processing. `auval`
proves discovery, initialization, properties, scope formats, and parameters, but
does not inject and verify MIDI for `aumi`; note-through, transformed output,
stopped-transport silence, and hot-swap balance require a real DAW smoke test.

For custom chains, the acceptance floor is:

1. loader success with no unexpected warnings;
2. deterministic probe output and exact note-on/note-off balance;
3. no allocation after preparation;
4. bounded output under maximum-density patterns and flush;
5. real-host MIDI input/output behavior, including stopped transport;
6. format validation appropriate to the shipped target.
