# Bounded graph voice binding

This source slice binds one authored `SubgraphTemplate` containing a versioned
in-process `Processor` to eight named slots in one `SignalGraph`. The caller
supplies a `VoiceBankOwner`; `MpeVoiceBankOwner<Voice>` retains one existing
`MpeVoiceAllocator<Voice>` and constructs one Processor per slot. The graph
retains the Processor instances, and the bank retains the owner until after
the graph is destroyed. Factories run only during control-thread preparation.
The callable and allocator address are transient and never enter the G-4
manifest, identity encoding, or saved state.

The bank resolves declared stereo output, the owner's exact named note and
expression endpoints, and named Processor parameter targets before publication.
It rejects null or repeated mutable slot identities. It uses G-5
`HostEventIngress` for absolute sample,
producer, sequence, overflow, late, and epoch checks. It dispatches all events
at each sample boundary before rendering that sample, splitting one prepared
graph render into intervals. Each interval passes a zero-channel input view
with the interval's exact frame count, as required by `ProcessBlock` even for
an instrument. A control-side failed candidate leaves the
previous bank live when the caller retains it. The example voice used by the
test chooses a triangle oscillator, a four-frame release, duplicated stereo,
and fixed `1/8` gain; these are fixture choices, not framework DSP defaults.

## Admission ordering and current limit

The 2026-11-06 G-4/G-6 packet ordered the A1 audio oracle before runtime
implementation. That formed a circular dependency for a production graph
render: the independent scalar oracle could be frozen first, but no installed
graph consumer could verify it until a binding existed. The lead authorized
implementing the binding and its independent audio oracle together. This
changes the work order, not the acceptance requirements. The data-only follow-
up carries only the generated vectors and in-process comparisons; the
independent verifier is a separate prerequisite.

This slice is **PARTIAL**. In its bounded MIDI 1.0 MPE profile, host ingress
accepts member and manager CC64 sustain, scoped CC120 all-sound-off, CC121
reset, and CC123 all-notes-off. Member pitch, pressure, and CC74 reach the
current note generation and change graph audio. Manager pitch, pressure, and
CC74 remain typed refusals because their zone expression composition is not
defined. The public consumer covers audible pedal/tail and expression traces
across multiple block partitions; its frozen A1 fixture covers seven stereo
sum, release, and steal cases in the installed consumer. The independent
scalar verifier and planted-negative checks are supplied separately. The
production-source mutation script requires positive,
broken, and restored outcomes for ingress, identity, pedal, and release
faults.

Saved bank activation, compatible edit, undo/redo, arbitrary graph shape,
hosted plugin formats, Linux/Windows execution, and the paced callback
allocation/lock/deadline matrix remain unqualified. Offline in-process audio
and an installed SDK consumer do not prove those runtime or platform lanes.

## Skipping idle voices

An eight-voice bank renders every slot on every block, including the slots
that have no note. For a bank that is mostly idle, that is wasted work. The
bank can skip the slots that have nothing to play.

The option is off by default. Pass it as the last argument of
`VoiceBankBinding::prepare`:

```cpp
auto bank = VoiceBankBinding::prepare(schema, definition, std::move(owner), "audio_out",
                                      48000, 256, status, midi::MpeConfig::standard_lower(),
                                      VoiceBankIdleSkip::SkipInactiveSlots);
```

With `SkipInactiveSlots`, a slot whose voice is not active is not rendered. Its
output is written as exact silence and its processor is not called. A voice in
its release tail is still active, so it keeps rendering until the release
ends. A note that arrives part-way through a block wakes its slot on the exact
sample, because the bank already renders a block in pieces that end at each
event.

### The contract you take on

The framework cannot prove that skipping a voice is harmless. You state it by
turning the option on. The statement is:

> At every note start, the voice's output must not depend on how many blocks
> it was rendered while its voice was inactive.

If that holds, a skipped slot produces exactly the same samples (bit for bit)
as a rendered one, at any block size.

### What the bank checks for you

- **Silence is confirmed first.** A slot is skipped only after it has been
  rendered while inactive and produced a block of all-zero samples, and has
  stayed silent for at least its declared latency plus its declared tail
  (`latency_samples()` plus `PluginDescriptor::tail_samples`). Any non-zero
  sample, including `-0.0f` and a decaying denormal, starts the count again, so
  a voice that never reaches exact zero is simply never skipped.
- **Changing a slot parameter** with `set_slot_parameter` makes that slot
  render at least once more before it can be skipped again, and `activate`
  does the same for every slot.
- **Unsafe shapes are refused.** `prepare` returns null with
  `VoiceBankBindingCode::InvalidManifest` (naming the slot) if a voice declares
  an audio input bus or audio-rate modulation, declares an unbounded tail
  (`tail_samples == -1`), or reports a negative latency. Note that a
  `PluginDescriptor` declares one stereo input bus by default, so an
  instrument voice must clear `input_buses` in its descriptor to qualify. The
  bank never turns the option off silently.

### Voices that must not enable it

Leave the option off if a voice keeps changing state while it is silent:

- a free-running LFO or noise source whose phase or seed is used when the next
  note starts;
- parameter smoothing that keeps moving while the voice is silent;
- anything that follows the transport or a clock while silent.

For example, a voice that starts each note at a phase taken from a counter that
runs on every rendered sample sounds different after a skipped stretch than
after a rendered one.

### Reading the saving

`VoiceBankBinding::idle_skipped_blocks()` returns how many voice-blocks were
skipped since `prepare`, summed over the eight slots (always 0 when the option
is off). A bank holding one note for N blocks with no other events skips
`7 * (N - 1)` voice-blocks: each idle slot renders once to confirm silence and
is skipped afterwards.

### Check before enabling

Render the same note script twice, once with `Off` and once with
`SkipInactiveSlots`, at several block sizes, and compare the two buffers with
`memcmp`. Include note on and off, a retrigger of the same key, a ninth note
that steals a voice, a release that crosses a block boundary, the sustain
pedal and all-sound-off. `test/test_voice_bank_binding.cpp` has such a test
("Voice bank idle skip renders identical audio with skipping on and off") that
you can copy for your own voice.
