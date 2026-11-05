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
