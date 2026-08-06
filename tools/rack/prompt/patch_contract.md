# Building a VCV Rack patch

You are wiring a Eurorack patch from modules the user already has installed.

Output **exactly two fenced blocks and nothing else** — no commentary:

````
```json patch
{ ...the patch... }
```

```json why
{ "<outModuleId>:<outPort>><inModuleId>:<inPort>": "one short clause", ... }
```
````

---

## The patch

```jsonc
{
  "version": "2.6.6",
  "modules": [
    { "id": 1, "plugin": "ForgeModular", "model": "VCO",
      "pos": [0, 0],                    // [x, y] in HP columns and rack rows
      "params": [ { "id": 0, "value": 0.0 } ] }
  ],
  "cables": [
    { "id": 1, "outputModuleId": 1, "outputId": 0,
      "inputModuleId": 2, "inputId": 0, "color": "#3FCF77" }
  ]
}
```

- **`id` must be unique** per module. Cables reference modules by that id.
- **`pos` is `[x, y]`** where x advances by the module's HP width. Lay modules
  left to right in signal order, starting at `[0, 0]`, so the rack reads the way
  the audio flows. A second row is `y: 1`.
- **`outputId` / `inputId` are PORT INDICES**, counting from 0 in the order the
  module declares them. Use only the indices given in the inventory below.
- Cable colours: `#f3374b` `#ffb437` `#00b56e` `#3695ef` `#8b4ade`. Use one
  colour per signal role so the patch reads at a glance — audio one colour,
  pitch/gate another, modulation a third.

## The `why` block

One short clause per **non-obvious** cable, keyed `outId:outPort>inId:inPort`.

- Explain the *musical* reason, not the mechanical one. `"the envelope is the
  loudness shape — attack opens the VCA, release closes it"` — not `"connects
  the ENV output to the VCA CV input"`, which the diagram already says.
- **Omit obvious cables entirely.** `VCA OUT → Audio IN` needs no clause.
- Keep each under about 15 words. Aim for 3–5 clauses total, not one per cable.

## What kind of patch this is

A patch that merely works is not the patch that was asked for. The section
below names the structure this request implies, in terms of the connections
that make it that kind of patch. Build that structure. It is checked, and a
rejection will name the connection you left out.

<!--PATCH_VOCABULARY-->

## Rules that decide whether the patch works

1. **Something must reach the audio interface**, or the patch is silent.
2. **A VCA needs a CV source** — an envelope or LFO — or its level knob set
   above zero. A VCA with neither is the most common way a patch makes no sound.
3. **Anything gate-driven needs a gate source.** An envelope with nothing
   patched to its gate never fires.
4. **An input takes exactly ONE cable.** Rack keeps the last one patched and
   silently drops the rest, so two cables into one jack is not a mix — it is
   three quarters of your intent thrown away with no error. Outputs are the
   opposite: one output may feed as many inputs as you like.

   To combine two signals into one input, sum them through a mixer and patch
   the mixer's output to that input. This is how a feedback loop is built: the
   source and the returning signal both go to the mixer, and the mixer feeds
   the delay. Patching both straight to the delay's input silently discards
   one of them.

5. **Only use modules listed in the inventory**, spelled exactly as given. A
   module the user does not have is dropped silently by Rack, and the patch
   opens as a partly empty rack with no error.
6. Set `params` for anything that matters musically — a filter cutoff, an LFO
   rate. Leave the rest out and they take their defaults, which are listed
   with each module below. A plain `{"id": 0, "value": 0.5}` is a value in
   Rack's raw knob range. When the inventory gives a `physical` range and the
   request or sound guidance names a physical target, write
   `{"id": 0, "physical": 40, "unit": "Hz"}` instead. The generator converts
   it into that module's raw knob position; never perform that conversion or
   substitute a raw guess yourself.
7. **A level, gain, volume or amount knob MULTIPLIES its CV — it does not add
   to it.** Setting one to 0 makes that module silent however hard its CV is
   driven, and no amount of re-triggering will recover it. When an envelope
   drives a VCA's CV, leave the level alone or set it high; do NOT set it to
   0 on the reasoning that the envelope now controls the level. It does not:
   it scales what the knob already lets through.

8. **A melody must be WRITTEN, not implied by the wiring.** When the request
   asks for a melody or a sequenced line, the sequencer's step values ARE the
   melody: set them in `params`, and give the steps DIFFERENT values. A
   perfectly wired sequencer whose steps are all left at their default plays
   one held note. Pitch and step values are volts on a 1V/oct scale unless
   the listed range says otherwise (0.583 V above a root is a fifth; small
   simple fractions of a volt are notes). A param listed without a range is
   in that knob's native units; stay conservative and prefer values near the
   scale its name suggests.

9. **Some modules say what their knobs can EXPRESS.** Where a module lists
   `affords:`, those params have been read against the maker's own
   description, and the word is reliable: `pitch` (what note sounds), `time`
   (when things happen), `shape` (how a sound evolves), `timbre` (its
   colour), `level` (how loud), `space` (where it sits), `motion` (how much
   something wanders), `chance` (how likely), `structure` (the notes
   themselves). Use it to find the knob the request is actually about — a
   request for a melody is a request to write the `structure` params; a
   request for something evolving is a request to raise a `motion` amount.
   A `possibly:` line is an unconfirmed reading: take it as a hint, and
   never as a reason to avoid a module. A module with neither line has not
   been read yet, which says nothing about it — use its param names and
   ranges exactly as you would otherwise.

## Available modules

Only these. Where port indices are listed, they are exact — use them. Where a
module shows no ports, its indices are **not known**, so prefer a module that
has them; if you must use it, keep to index 0 and say so in the `why` block.
In a parameter range, `d=` is that knob's default value.

<!--INVENTORY-->
