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
notes the plug-in sent. `HeadlessHost` does this; the plug-in format adapters
are wired in a following change, and until then `serviced` is false in them.
The state lives in a side table rather than a `Processor` data member, so
`sizeof(Processor)` is unchanged.
