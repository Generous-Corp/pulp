# MPE configuration messages

`MpeVoiceTracker` follows the messages an MPE controller sends to describe its
layout, and `pulp/midi/mpe_config_messages.hpp` builds the same messages for
sending to a device. Both sides use plain MIDI 1.0 control changes.

## What the tracker reads

| Message | Meaning |
|---|---|
| RPN 6 on channel 1, data entry MSB = N | Lower zone: manager on channel 1, N member channels |
| RPN 6 on channel 16, data entry MSB = N | Upper zone: manager on channel 16, N member channels |
| RPN 0 on a manager channel | Manager pitch-bend range for that zone, in semitones plus cents |
| RPN 0 on any other channel | Member pitch-bend range for that channel only |

An RPN is selected with CC 101 (MSB) and CC 100 (LSB), then set with CC 6 (data
entry MSB) and optionally CC 38 (data entry LSB). The value takes effect on CC 6
and CC 38 refines it, so a sender that omits CC 38 works.

- **Per-channel state.** Parameter selection is tracked separately on each of the
  16 channels, so interleaved messages on different channels do not mix.
- **NRPN is not RPN.** Selecting an NRPN (CC 99 or CC 98) cancels the RPN
  selection on that channel. A data entry that follows is never read as a
  pitch-bend range or a zone count. RPN 127/127 deselects.
- **Zones share channels 2 to 15.** When both zones exist their member counts
  add up to at most 14. The zone being configured wins, and the other zone gives
  up the channels it can no longer keep. A zone with 15 members leaves no room
  for the other zone.
- **A configuration message resets.** The affected zones return to the default
  ranges (48 semitones for members, 2 for the manager), their zone-wide state and
  per-channel expression clear, and notes on channels that left a zone are
  released through `on_note_off`.
- **Observing it.** `on_config_change` fires after every configuration message,
  and `config()` returns the new layout.

`set_follow_configuration_messages(false)` makes the tracker ignore these
messages and keep whatever `set_config()` was given.

## Sending

```cpp
#include <pulp/midi/mpe_config_messages.hpp>

// Lower zone with 8 members, upper zone with 4.
auto layout = pulp::midi::mpe_zone_layout_messages(pulp::midi::MpeConfig::dual(8, 4));
pulp::midi::send_mpe_messages(output, layout);

// Ranges go after the layout, because a configuration message resets them.
auto ranges = pulp::midi::mpe_zone_pitch_bend_range_messages(
    pulp::midi::MpeConfig::dual(8, 4).lower_zone, /*manager*/ 2.0f, /*member*/ 24.0f);
pulp::midi::send_mpe_messages(output, ranges);
```

The builders return fixed-size lists, so they do not allocate and are safe to call
anywhere.
