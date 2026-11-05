# MIDI panic

A panic silences a receiver: it ends sounding notes, drops sustain, and puts
displaced controllers back at rest. `pulp/midi/panic.hpp` provides the message
generation as plain functions over caller-owned storage: no allocation, no
locks, no state, so it is safe on the audio thread and testable without a host.

## What a panic sends

Per channel, in this order:

| Message | Meaning |
|---|---|
| CC 64 = 0 | Damper (sustain) off, so pedal-held notes release first |
| CC 123 = 0 | All Notes Off: release held notes through the normal release |
| CC 120 = 0 | All Sound Off: stop immediately, release tails included |
| CC 121 = 0 | Reset All Controllers |
| Pitch bend 8192 | Pitch wheel centre |

CC 120 and CC 123 are different on purpose. All Sound Off is the immediate
stop; All Notes Off lets tails ring out. A default panic sends both, which also
covers receivers that implement only one. `PanicScope::release_only()` omits
CC 120 for a panic that keeps release tails.

```cpp
#include <pulp/midi/panic.hpp>

std::array<pulp::midi::MidiEvent, pulp::midi::kMaxPanicMessages> messages;
auto n = pulp::midi::panic_messages(pulp::midi::PanicScope{}, messages,
                                    /*sample_offset=*/0);
```

`PanicScope::channel_mask` selects channels (bit n = channel n) and each kind
has a switch. `panic_message_count(scope)` gives the size to reserve;
`panic_messages` cuts at a whole-event boundary if the span is too small.

## Note-offs

`held_note_offs(keys, out, channel_mask, sample_offset)` sends a note-off for
exactly the notes a `MidiKeyboardState` reports as held, and nothing for a key
that is not down. Feed the state with `keys.process(event)` as events pass
through `process()`.

`all_note_offs(out, channel_mask, sample_offset)` is the brute-force fallback
(up to `kMaxNoteOffs` = 2048 events) for hosts and formats that bypass the
tracker.

## Recognising panic messages

`classify_panic_message(event)` returns the `PanicMessageKind` or no value.
Only the exact messages count: CC 123 with a nonzero value, or a pitch bend off
centre, is not a panic message.

## Panicking a processor

`Processor::request_panic(mode)` asks a plug-in to silence itself. It is safe
from any non-real-time thread (a UI button, a menu item, a script): it records
the request and returns, and the host services it before the next audio block.

```cpp
processor.request_panic();                          // hard stop (default)
processor.request_panic(format::PanicMode::release); // let tails ring out
auto caps = processor.panic_capabilities();          // {serviced, midi_out}
```

| Mode | Voices | MIDI sent downstream |
|---|---|---|
| `immediate` (default) | hard-reset, release tails cut (CC 120 semantic) | note-offs for held notes, then sustain off, CC 123, CC 120, CC 121, pitch bend centre on every channel |
| `release` | enter their normal release, tails ring out (CC 123 semantic) | the same, without CC 120 |

A request that arrives while another is pending keeps the harder mode.

### Implementing the hook

A plug-in that owns voices overrides `on_panic(PanicMode)` to stop them.
`midi::Synthesiser::panic(bool immediate)` is the one-line implementation:

```cpp
void on_panic(format::PanicMode mode) override {
    synth_.panic(mode == format::PanicMode::immediate);
}
```

`on_panic` runs on the audio thread before `process()`, so it must not
allocate or lock. The default does nothing.

### What the host provides

`panic_capabilities()` reports what a request can do in the current host:

- `serviced`: an adapter is running requests. A processor not attached to a
  servicing adapter records the request and nothing happens; check this flag
  before showing a panic control.
- `midi_out`: the adapter has MIDI output, so the panic messages and note-offs
  reach downstream devices. When false, only the plug-in's own voices are reset.

An adapter attaches with `PanicState::attach(midi_out)`, calls
`service_panic(processor, state, midi_out)` before `process()` and
`state.observe_midi_out(midi_out)` after, so the note-offs cover exactly the
notes the plug-in sent. The state lives in a side table rather than a
`Processor` data member, so `sizeof(Processor)` is unchanged.

| Host | `serviced` | `midi_out` |
|---|---|---|
| `HeadlessHost` | yes | the descriptor's `produces_midi` |
| CLAP | yes | `produces_midi` |
| VST3 | yes | `produces_midi` |
| AU v2 effect (`aufx` / `aumf`) | yes | `produces_midi` (an `aufx` declares none, so voices only) |
| AU v2 MIDI processor | yes | `produces_midi` |
| AU v2 instrument (`aumu`) | yes | no: the adapter has no host MIDI output path yet |
| AU v3 | yes | `produces_midi`, delivered through the host's `MIDIOutputEventBlock` when it installs one |
| Standalone app | yes | no: there is no MIDI output device path |

A bypassed AU v2 effect skips `process()`, so a request raised while bypassed
is serviced on the first block after bypass is released. Formats not listed
(LV2, AAX, web) do not service requests: `serviced` is false there.

### A panic bigger than one block's MIDI buffer

A full panic is up to 80 channel messages plus one note-off per held note, and
an adapter's per-block MIDI buffer has a fixed capacity. When the buffer fills,
`service_panic()` writes what fits and keeps the rest: notes not yet ended stay
in the tracker, and the channel messages resume at the first one not written.
The following blocks continue the panic without a new request, and the plug-in's
voices are reset once, when the request is first serviced. A new request during
a carried panic keeps the harder of the two modes.

`PanicOutcome::events_deferred` and `complete` describe one block, and
`PanicState::deferred_events()` is a running total readable from any thread
(the audio thread does not log). A total that keeps rising on a large held set
means the adapter's MIDI buffer is small for the panic.
