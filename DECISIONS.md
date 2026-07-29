# Forge Modular — decisions and why

Choices that were arguable, with the reasoning that settled them. Written so
that anyone revisiting one can tell whether the reasons still hold, rather than
re-deriving the argument from scratch or assuming a decision was arbitrary.

Each entry names what would change our mind, because that is usually the part
that gets lost.

---

## Ship as an audio effect, not an instrument

**Rack Pro wants the instrument slot.** If Forge Modular takes one too, a whole
track is spent on a chat window, and the two things a person wants side by side
end up on separate channels.

An insert slot puts them on the *same track*: Rack Pro in the instrument slot,
Forge Modular on an insert below it. One track, both windows. Every DAW has
inserts, so this behaves the same in Logic, Live, Reaper, Bitwig and Studio One.

The category is also a claim the host acts on. `Instrument` tells a DAW to
expect audio out; Forge Modular produces none, so the DAW would be waiting on
silence. `Effect` passing audio through untouched is honest about what it does.

It declares zero latency and no tail, so no host adds delay compensation for a
plugin that does nothing to the signal.

**Would change our mind:** a host where insert slots cannot open a large
persistent editor window comfortably, or where an insert on an instrument track
is awkward enough that people would rather spend the channel.

## Not a MIDI effect — for now

`MidiEffect` is the most *semantically* honest category. Logic's MIDI FX slot
sits before the instrument, entirely outside the audio path, which is exactly
right for something that makes no sound and touches no signal.

It loses on portability. That placement is Logic-shaped: Live, Reaper and
Bitwig have no equivalent slot in the same position, so choosing it optimises
for one host and reads as odd in the others. Cross-DAW consistency matters more
for v1 than being philosophically correct in one DAW.

**Would change our mind — and this is a real, foreseeable trigger:** wanting to
generate a patch *informed by the MIDI on the track*. "Build me something that
plays this clip" requires seeing the notes, and a MIDI effect sees them where an
audio effect placed after the instrument never will. If that feature is ever
wanted, the category should flip. It is one field in the descriptor plus a
re-validation pass, not a rewrite — but a shipped plugin changing category is
disruptive for existing sessions, so it is better decided before launch than
after.

## All three formats — AU, VST3, CLAP

Rack Pro ships in all of them, so whichever format a person's DAW is built
around, they are served. A plugin with no DSP is the cheapest possible thing to
ship three times, and Pulp's adapters already carry each one. There is no
argument for picking a subset beyond saving build time, which is not worth
someone's DAW being unsupported.

VST2 is deliberately excluded: it is deprecated, and Rack's own Host module
supporting it is not a reason for us to.

## Context decides where output goes

Forge Modular exists to put things into Rack, so it constantly has to answer
*which* Rack. The rule is that the shell decides:

- **Running as a plugin in a DAW** — the user is in that DAW, so the target is
  the Rack Pro instance beside them. Sending them to a separate standalone Rack
  would be answering a question they did not ask.
- **Running standalone** — the target is standalone Rack, launched if it is not
  already up.

Everything else is fallback, and when nothing can be found we say so plainly
rather than guessing. A setting to override can come later; it should not be
needed to make the common case work.

### The asymmetry that shapes this

**Standalone Rack can be launched and handed a patch file. A Rack Pro plugin
instance can be neither.** No plugin can instantiate another plugin in its host,
and none can tell its host to open a file. This is not a gap to engineer around
— it is what plugin isolation means.

So the two paths differ in how far we can take the user:

| Context | Launch Rack? | Hand it a patch? |
|---|---|---|
| Standalone app | yes | yes, as a launch argument |
| Plugin in a DAW | no | no — prepare it, then say where it is |

Pretending otherwise would produce a button that silently does nothing, which
is worse than a clear "here is your patch, open it in the Rack instance on this
track."

## Rack missing is a blocking state, but an escapable one

Without Rack there is genuinely nothing the product can do, so a blocking modal
is right rather than letting someone prompt into a void.

It carries a **re-check** action rather than being a dead end, and a way past on
insistence. Detection can be wrong — a non-standard install location is enough —
and a detection bug that permanently locks someone out of software they paid for
is a worse failure than a false negative they can dismiss.

## Cardinal is out

Cardinal is a Rack port that runs as a plugin, which sounds like an easy second
target. It is not: *"Installing new modules on a Cardinal build is not
possible"*, by design. Generating new modules is the entire product, so the two
are incompatible at the premise.

Generating *patches* for Cardinal would work — a patch is JSON naming module
slugs, and the inventory, explanation and lint machinery is format-agnostic. If
Cardinal is ever revisited, that is the part to revisit.

## Module layout stays compiled in

A module's panel geometry is emitted into generated C++, so moving a knob needs
a rebuild. Reading layout from a JSON resource at load would make panel edits
instant, which a future panel designer would want.

Deferred, because the designer does not exist and data-driven layout adds a
runtime failure mode — missing or stale JSON in the bundle — for no present
gain. An earlier version of this note claimed retrofitting would be painful;
that was wrong. Every module is generated, so retrofitting is a change to one
emitter function and a regeneration, with no hand-written code to migrate.

**Would change our mind:** building a panel designer, at which point live edits
matter more than the extra failure mode.

## The DSP gate checks that Pulp's DSP was used, not that the right class was

A module reaching for `SmoothedValue` passes even where `SimpleMixerT` would
have been the better fit. The gate can see that the SDK was used at all; it
cannot judge whether the choice was apt.

This is deliberate rather than unfinished. Judging aptness means encoding which
class belongs to which kind of module, which is exactly the hand-maintained
mapping the header-derived vocabulary exists to avoid — and it would be wrong
often, since a mixer that de-zippers its gains with `SmoothedValue` and sums by
hand is arguably better than one that reaches for a mixer class and clicks.

The gate catches the failure that actually happened: a module using none of
Pulp at all, because the vocabulary in the prompt was wrong or too awkward to
use. That failure is unambiguous. "Used a suboptimal class" is a judgement, and
a gate that makes judgements will make them badly.

**Would change our mind:** evidence that generated modules routinely pick a
technically-valid but poor class, in a pattern specific enough to name.
