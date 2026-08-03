# Patch language — contract v1

A text notation for signal-flow graphs: what nodes exist, what is connected to
what, and how each node is set.

**Status:** working for VCV Rack patches (`tools/rack/patch_lang.py`), 138/138
real patches round-trip. Generalisation to Pulp plugins, standalone apps and
core DSP graphs is designed and unbuilt — see the plan in the private planning
repo.

This is a sibling of [`pulp-dsl-v1.md`](pulp-dsl-v1.md), not a replacement.
`pulp-dsl` is how a *behaviour* language (FAUST, Cmajor, JSFX) plugs into
Pulp's `Processor`. This is how a *topology* is written down. They meet where a
node in a patch is a unit some DSL defined.

---

## Why this exists

The short version: **a notation whose grammar cannot express a mistake removes
that mistake from every producer at once** — model, script and human.

Every serialisation we had was machine-oriented. A `.vcv` is JSON with integer
port indices and panel coordinates; a `.pulpgraph` is JSON. Both are fine as
transport and impossible as authoring, and asking a generator to emit one
directly means asking for things it cannot know. These are real defects from
this codebase, each *unrepresentable* in the notation:

| What happened | Why the notation prevents it |
|---|---|
| Generated patches with panels overlapping by 2HP; a live run burned a model call being rejected for it | there is no way to write a coordinate — layout is computed on the way out |
| Port index off by one → a patch that loads and makes the wrong sound, with no error | ports are named and checked against the vocabulary |
| A six-channel mixer reported ONE input to the generator for months | the vocabulary is asked once, by everyone |

The second reason: **the artifact becomes reviewable and editable.** A `.vcv`
diff is noise; a notation diff is a sentence. Change `fourpole.Resonance = 0.88`
and re-render — no model call. That is the difference between a generator and a
tool.

The honest argument against, which the maintenance section answers: a notation
that lags the things it describes is worse than none, because it lies with
authority.

## What it does and does not describe

| Layer | In the notation | Why |
|---|---|---|
| **Topology** — what connects to what | yes | the thing a graph *is* |
| **State** — values; for plugins, parameter declarations and bindings | yes | a patch without its values is a wiring diagram, not a patch |
| **Behaviour** — what a filter *is*, sample by sample | **no** | that is `pulp::signal`, or a DSL via `pulp-dsl` |

The line is between *configuring* a unit and *defining* one. Configuring is
declarative, finite and per-instance. Defining is code.

Two checks that the line sits right: a Rack module is an opaque binary whose
behaviour cannot be described at all, so any design requiring behaviour could
not express a Rack patch; and Pulp already hosts three behaviour languages
through `pulp-dsl`, so a fourth would compete with what it deliberately
integrates.

## The notation

```
lfo   : ForgeModular/LFO
seq   : ForgeModular/SEQ
quant : Fundamental/Quantizer
vco   : ForgeModular/VCO
out   : Core/AudioInterface2

lfo.Square >> seq.Clock
seq.'Pitch CV (1V/oct)' >> quant.'1V/octave pitch' >> vco.'1V/oct pitch'
vco.Sawtooth >> out.'To "device output 1"', out.'To "device output 2"'

vco.'Pulse width' = 0.5
```

Read aloud: "LFO square into seq clock. Seq pitch into quantizer pitch into
VCO volt-per-octave."

Those are the ports' real names, which is not a stylistic point. An earlier
draft of this block said `lfo.SQR`, `seq.CV` and `vco."V/OCT"` — abbreviations
that read like module panels and exist nowhere. It passed for as long as it
did because the port map had no entry for these modules at all, so the checker
had nothing to contradict; the moment the vocabulary became complete the
example stopped parsing. Rule 2 below is what caught it.

| Form | Means |
|---|---|
| `name : Plugin/Module` | declare a node |
| `a.OUT >> b.IN` | connect |
| `a.OUT >> b.IN >> c.IN` | chain — `b`'s output is **inferred** when it has exactly one |
| `a.OUT >> b.IN, c.IN` | fan out |
| `name.PARAM = 0.5` | set a value |
| `name.PORT#2` | the **second** port of that name |
| `"a port"` / `'a port'` | quote a name that is not an identifier |
| `# ...` | comment; the first one is kept as the request |

Two ordering rules the renderer follows: **bill of materials, then wiring, then
tuning** (values between the declarations and the cables buried the topology),
and the shortest spelling that parses back — an index suffix only where a name
repeats on that module, a chain port only when ambiguous. Where a name does
repeat, every port of that name is numbered (`TRIG#1` and `TRIG#2`, not bare
`TRIG` and `TRIG#2`), so a pair reads as one control with two channels.

### Sigils

Definitional operators point **left** (`:`, `=`, and the planned `<-`); flow
operators point **right** (`>>`, and `,` for fan-out). Use that rule to settle
future syntax questions.

## What makes it different from a generic graph text format

Each of these depends on knowing the *real* nodes, which is why the
`Vocabulary` seam is the design's centre rather than a detail:

1. **Checked.** Every port is verified against the installed inventory. A typo
   names the line and lists the ports that exist, instead of producing a patch
   that loads into silence.
2. **Inferring.** A chain continues through a module's single output without
   naming it; with several it refuses and lists them. A fixed-primitive
   language cannot do this.
3. **Round-tripping.** Machine format → text → machine format, compared on port
   **indices and values**.

## Maintenance — how this is kept from falling behind

The load-bearing part. A notation rots when the things it names change and
nothing notices.

**Rule 1 — a vocabulary never carries its own copy of the node list.** It
answers "what nodes exist and what ports do they have" from the same source the
runtime uses: manifests and the CARTOG scan for Rack, the node registry for
`SignalGraph`, the descriptor for a plugin. The six-channel-mixer bug was
exactly a second copy disagreeing with the first.

**Rule 2 — every documented example is executed.** The suite extracts the block
between `EXAMPLE-BEGIN` and `EXAMPLE-END` in the implementation's docstring and
parses it. Two drafts of that example invented port names that do not exist
(`out.LEFT`, `quant.IN`); both were written by reasoning about what a port is
probably called rather than reading the inventory, and neither was caught until
the example was run. **When you change this document's example, it must be
copied from a real render or it will be wrong.**

**Rule 3 — the corpus is the contract.** `test_patch_lang.py` round-trips every
patch on the machine, comparing indices and values. A grammar change that
breaks any of them is a breaking change.

**Rule 4 — the renderer must only print what the parser accepts.** Verified
directly: every value in the corpus is checked to survive `%g` and the grammar.
This caught `1e-05`, which the renderer printed and the parser rejected, and
which no patch in the corpus happened to contain.

**Planned, with the core extraction:** a path map from vocabulary sources to
the corpus that proves them (in the shape of `skill_path_map.json`), a
pre-push/CI gate that fails a diff touching a vocabulary without touching its
corpus, and a `# flow 1` version header — because without a version there is no
honest way to reject a file from the future.

## Using it

```sh
patch_lang.py show  <file.vcv>            # machine format -> text
patch_lang.py build <file.pat> <out.vcv>  # text -> machine format
patch_lang.py check <file.pat>            # parse and lint only
```

`build` lays panels out and lints before writing, so a hand-written patch gets
the same checks a generated one does.
