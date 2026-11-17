# D5: sustain semantics and the voice retirement policy

Status: **PARTIAL**. The bank's existing sustain and channel-mode semantics
are written down as a contract and pinned by tests, and an opt-in
`DrainRetired` retirement policy is built and tested in-tree. The default,
`HardCut`, is unchanged. What is not built is listed under "Not done".

## What a reset does to sounding voices

`pulp::host::VoiceBankBinding`
(`core/host/include/pulp/host/voice_bank_binding.hpp`) moves to a new epoch on
`activate()` and on `commit_event_reset()` (the voice step of the S4 reset
transaction, so every `HostTimelineBinding` seek, loop, clock change and
requested reset takes this path too). `VoiceBankRetirementPolicy`, set with
`set_retirement_policy()` between blocks, decides what happens to the voices
that are sounding at that moment.

| Policy | Behaviour |
|---|---|
| `HardCut` (default, `capacity` 0, `tail_frames` 0) | Every voice is cut on the reset through the owner's `reset()`, exactly as before. |
| `DrainRetired` (`capacity` 1 to 8, `tail_frames` 1 to 2^20) | Each sounding voice is retired: a held or pedal-held voice is released, an already releasing voice keeps releasing, and it may render for at most `tail_frames` further frames before it is cut on that exact frame. |

Rules of `DrainRetired`:

- At most `capacity` voices drain at once. Drains still running from an earlier
  reset count against it and keep their original deadline (a second reset
  never extends a tail). Every further sounding voice is refused a drain and cut
  on the reset, scanning slots in ordinal order. Refusals are counted.
- A retired voice belongs to no epoch. Old-epoch tokens are `StaleEpoch`; a
  token re-stamped with the new epoch for a retired generation is
  `StaleGeneration`. The new epoch's sustain pedal and CC121 do not touch it.
- All Sound Off covering its channel cuts it, and a new note that steals its
  slot ends it. Neither is cut later in the slot's name: the deadline is tied to
  the retired generation.
- The deadline is counted in rendered frames and `render()` splits its graph
  slices at it, so the cut lands on the same frame for every block partition.
- The owner's `reset()` is called only when no voice is left draining, so a
  reset with nothing sounding behaves exactly as `HardCut`.
- The retirement table is a fixed array of eight entries owned by the bank.
  Reset, drain, cut and the counters neither allocate nor lock.
- `set_retirement_policy()` refuses an out-of-range policy and any change while
  a drain is running, leaving the current policy in place.

`retirement_stats()` reports voices draining now and, since prepare, voices
drained, refused, finished before their tail, cut at their tail, and
interrupted by All Sound Off or a steal.

### Never persistent

`capture_state()` records only declared slot parameters. Retired voices, their
deadlines, active tokens, epochs, pedal holds and opaque DSP tails are not
representable in `VoiceBankState`, and `restore_state()` revives none of them.
The retirement policy is bank configuration, not state.

## Sustain and channel-mode contract

The header comment above `VoiceBankBinding` is the normative text. In short:
CC64 at 64 or above is down; a member pedal covers its channel and a manager
pedal its zone; a note-off for a sustained note (MIDI, CC123 or a deferred
NoteOff) holds that exact generation; a pedal rise releases each hold no pedal
covers any more, once; a stolen hold never releases its replacement; CC121
lifts its scope's pedal and resets its expression; CC120 cuts its scope at once
whatever the pedal; channel modes with a non-zero value and unsupported
controllers are refused before the block changes anything; a reset lifts every
pedal and clears every hold.

## Evidence

`test/test_voice_bank_retirement.cpp` (`pulp-test-voice-bank-retirement`), all
through the public binding with a test voice whose output names its note:

- HardCut default: the default policy is `HardCut`/0/0, an unconfigured bank
  and one set to `HardCut` render identical samples, every old voice is silent
  from the reset frame, all counters stay zero; positive control that the
  script is loud just before the reset.
- Drain across partitions: seven partitions (1, 7, 16, 32, 64, 13/5/29,
  3/61/2) give the same samples and counters. For the drained frames the
  output equals the HardCut run plus an independent per-voice expectation (held,
  pedal-held, already releasing and short-release voices); from the deadline on
  it equals the HardCut run; one frame before the deadline it differs by more
  than 1.0 (negative control). Counts: 5 drained, 1 finished, 4 cut at tail.
- Capacity: capacity 2 drains slots 0 and 1 and refuses 3; a second reset while
  both drain keeps their deadline and refuses the new epoch's note.
- Policy refusals: capacity 0 and 9, tail 0 and 2^20 + 1, HardCut with a
  capacity or tail, an unknown mode, inside an open block, and while draining;
  boundaries accepted; a refusal keeps the current policy.
- Tokens, pedal and CC121: old-epoch token `StaleEpoch`, re-stamped token
  `StaleGeneration`, new-epoch token accepted (positive control); CC121 on the
  retired voice's channel leaves its bend; a new-epoch pedal press and lift
  leaves it releasing.
- All Sound Off and a steal end a drain early and are counted; the stolen
  slot's old deadline does not cut its replacement.
- `activate()` follows the same policy.
- State captured during a drain equals state captured before the reset;
  restoring it onto a fresh bank revives no voice, drain or policy.
- Sustain: a deferred NoteOff under a member pedal is held and released by a
  lift to 63; without the pedal it releases at once.
- No allocation across reset, drain, refuse and cut blocks
  (`RtAllocationProbe`, with its self-check).

`tools/scripts/confirm_failure.sh` on the committed tree: see the commit
message for the planted breaks and their verdicts.

Existing suites still pass: `pulp-test-voice-bank-binding` and
`pulp-test-host-timeline-binding`.

## Not done

- No crossfaded bank swap and no tail handoff between two banks: draining is
  within one bank across its own epochs.
- Which voices drain when capacity is short is slot order, not age or loudness.
- Under steal modes other than `Oldest`, a new note may steal a held new-epoch
  voice while a retired voice keeps draining.
- Lock freedom is by construction only; the RealtimeSanitizer lane is not run.
- No installed-SDK consumer exercises `DrainRetired`; the evidence is the
  in-tree Catch2 suite on macOS arm64.
- No real-DAW check.
