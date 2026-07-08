# Base vs. Modulated Values

Pulp separates a parameter's **base value** (what the user set / the host
automates) from its **modulation offset** (what an LFO, envelope, or mod source
adds on top). The two are read together as one **effective value**. This page is
the contract for how that works — written to answer a recurring integration
question: *is the base-plus-modulation read uniform across all parameters, or
only on parameters explicitly declared as modulation targets?*

**Short answer:** the read is **uniform across every parameter**. There is no
per-parameter opt-in required to get a base-plus-modulation read. A separate,
higher-level layer (`modulation_lane`) governs *routing validation* — which
source is allowed to drive which target — and that layer is per-declared-target.
The two are different altitudes and are easy to conflate.

---

## Layer 1 — the effective-value read is uniform

Every `ParamValue` in a `StateStore` carries both a base value and a modulation
offset. The effective read is:

```
effective = base + mod_offset
```

exposed on both the parameter and the store:

```cpp
// core/state/include/pulp/state/parameter.hpp
float get_modulated() const;   // base + mod_offset (relaxed atomics, RT-safe)

// core/state/include/pulp/state/store.hpp
float get_modulated(ParamID id) const;   // same, by id
void  set_mod_offset(ParamID id, float offset);
void  add_mod_offset(ParamID id, float delta);
void  reset_all_mod();
```

This holds for **every** parameter — there is no flag a parameter must set to
participate. A parameter that is never modulated simply has `mod_offset == 0`, so
`get_modulated()` equals `get_value()`. That means UI affordances that visualize
modulation — **mod rings, meters, ghost handles** — can bind uniformly against
`get_modulated()` across your whole parameter set without special-casing which
parameters are "modulatable."

Threading: `get_value()` / `get_modulated()` are lock-free and audio-thread-safe
(relaxed atomics). `set_mod_offset` / `add_mod_offset` are the modulation-writer
side; a synth writes the offset per block (or per sample for audio-rate mod) and
the audio path reads `get_modulated()`.

## Layer 2 — modulation *routing* is per-declared-target

`core/state/include/pulp/state/modulation_lane.hpp` is a **separate** layer. It
does not provide the effective-value read; it describes and validates a
**routing lane** from a `ModulationSource` to a `ModulationTarget`:

```cpp
struct ModulationTarget {
    ParamID          param_id;
    ModulationScope  scope;        // Global / Voice / Note / GraphNode
    ParamRate        param_rate;   // ControlRate / AudioRate
    bool             modulatable = true;
    bool             writable    = true;
    std::string      units;
};

ModulationLaneValidation validate_modulation_lane(const ModulationLane&);
```

Here `ModulationTarget::modulatable` (and `writable`, and scope/rate
compatibility) **is** per-declared-target: `validate_modulation_lane()` rejects a
lane whose target is not `modulatable`, not `writable`, scope-incompatible, or an
audio-rate source aimed at a control-rate target. This is the layer you use when
you want Pulp to **own and validate a modulation matrix** — e.g. a
user-assignable mod-matrix UI where not every parameter should be a legal
destination.

So both statements are true, at different layers:

| Question | Layer | Answer |
|---|---|---|
| Can I read `base + modulation` for any parameter? | `StateStore::get_modulated` | **Yes, uniform** — no per-param declaration |
| Can any source route to any target? | `modulation_lane` | **No** — per-declared-target validation gate |

If all you need is uniform mod-ring / meter binding, you only need Layer 1. You
reach for Layer 2 only when you want Pulp to validate/own the routing graph.

---

## Pattern: giving a destructive parameter a base/modulated pair

A common port issue: an engine currently writes motion **destructively** —
the LFO/envelope overwrites the parameter's stored value, so there is no base to
show a mod ring against (the "base" and "modulated" values are the same number).

The fix is entirely client-side and needs **no Pulp change**: split the write.

```cpp
// Before — destructive: the base IS the modulated value, no pair exists.
store.set_value(cutoff_id, base_cutoff + lfo * depth);

// After — base and modulation are distinct; get_modulated() reconstitutes the pair.
store.set_value(cutoff_id, base_cutoff);          // user/host-owned base
store.set_mod_offset(cutoff_id, lfo * depth);     // mod-source-owned offset
// UI binds a mod ring to store.get_modulated(cutoff_id); DSP reads the same.
```

`add_mod_offset` accumulates multiple sources into one offset; `reset_all_mod()`
clears every offset at the top of a block before re-accumulating. Once a
parameter is written this way, `get_modulated()` yields the base-plus-modulation
pair uniformly — same as every other parameter.

## Related

- CLAP modulation maps directly onto this: the host's per-note/voice modulation
  becomes `set_mod_offset` / `add_mod_offset`, and `get_modulated()` is what the
  DSP reads.
- `modulation_lane.hpp` — routing validation for a modulation matrix.
- `core/audio/include/pulp/audio/voice_modulation_buffer.hpp` — per-voice
  modulation accumulation for polyphonic synths.
