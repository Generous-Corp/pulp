# DAW host-quirks policy

Pulp's format adapters consult a `pulp::format::HostQuirks` struct at
init time to switch between defensive defaults (always-on) and
host-gated behaviors (only when a specific DAW + version is detected).

The full catalog of accommodations + the per-host headers under
`core/format/include/pulp/format/host_quirks/<host>.hpp` are the
authoritative source. This page is the plugin-author surface — how to
dial in exactly the accommodations you trust.

## Three validation tiers

Pulp tags every individual quirk flag with a `pulp::format::QuirkStatus`
tier so plugin authors can opt out of accommodations that haven't been
bench-confirmed in the named DAW yet:

| Tier            | Meaning                                                                                              | Example fields                                                  |
|-----------------|------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------|
| `Validated`     | Exercised by a Pulp regression test under the named DAW.                                             | `synthesize_bypass_parameter`, `clamp_latency_to_nonneg`, `silence_unsupported_bus_arrangements` |
| `Speculative`   | Implemented per host docs + reproducer report; not yet bench-confirmed.                              | `cubase10_async_view_resize_queue`, `live_vst3_canresize_ignore`, `wavelab_state_blob_fallback` |
| `LessonOnly`    | Catalog entry only; carried so the enum stays in lock-step with the catalog. No fix wired yet.       | `reaper_process_while_bypassed`, `pro_tools_aax_*`, `au_v3_bypass_dual_tracking` |

The authoritative current tier of every flag lives in the constexpr
`pulp::format::kHostQuirksMeta` instance. When a quirk graduates from
`Speculative` to `Validated`, update `kHostQuirksMeta` and the row in
`planning/host-quirks-log.md` together (the log records the
in-DAW evidence + the regression test that validates it).

## Default policy (build-time)

The CMake cache variable `PULP_HOST_QUIRKS_DEFAULT_POLICY` selects what
`pulp::format::detect_quirks()` returns by default:

```bash
# Only Validated accommodations fire — Speculative + LessonOnly stay at
# their defaults. This is the SHIPPED Pulp default: enforce what's been
# bench-validated, evidence-gate the rest.
cmake -S . -B build -DPULP_HOST_QUIRKS_DEFAULT_POLICY=validated_only

# Apply every detected quirk regardless of tier (pre-enforcement behavior).
cmake -S . -B build -DPULP_HOST_QUIRKS_DEFAULT_POLICY=all

# Diagnostic build: no accommodations at all (including cheap defenses).
cmake -S . -B build -DPULP_HOST_QUIRKS_DEFAULT_POLICY=off
```

The flip to `validated_only` is behavior-neutral today: every quirk an
adapter currently consumes (`clamp_latency_to_nonneg`,
`silence_unsupported_bus_arrangements`, `synthesize_bypass_parameter`) is
`Validated`, so it survives the filter. It evidence-gates *future*
host-specific quirks — a `Speculative` row won't fire by default until it
earns `Validated` status.

The policy is compiled into the `pulp::format` library. Plugins built
against that library inherit whichever policy was chosen at configure
time, with **no runtime overhead** beyond a single field reset.

## Runtime opt-in / opt-out

A plugin author can ignore the build-time default entirely by
constructing a `HostQuirks` themselves and feeding it to the adapter
init:

```cpp
#include <pulp/format/host_quirks.hpp>

using namespace pulp::format;

// Detect the DAW + apply only Validated accommodations.
auto quirks = make_quirks_for_validated_only(detect_host_info().type,
                                             detect_host_info().version);

// Or build a custom filter — e.g. "Validated + Speculative, no Lessons":
auto q = make_quirks_for(host_type, host_version);
apply_filter(q, QuirkFilter{
    .allow_validated   = true,
    .allow_speculative = true,
    .allow_lesson_only = false,
});

// Or hand-edit a single field after the factory ran.
auto q2 = make_quirks_for(host_type, host_version);
q2.reaper_process_while_bypassed = false; // override one row
```

`apply_filter()` resets each field whose tier is not allowed back to
its `HostQuirks{}` default — including the numeric
`logic_au_channel_probe_cap`, which reverts to the cross-host cap of
64. Cheap defenses (currently `Validated`) survive the
`kQuirkFilterValidatedOnly` shortcut; if you also want to drop the
cheap defenses, pass `kQuirkFilterOff`.

`apply_filter()` is idempotent — applying the same filter twice is a
no-op on the second call.

## Runtime policy: env var + API override (no recompile)

The `make_quirks_for(...)` / `apply_filter(...)` route above is the
fully manual surface. Since the host-quirks **enforcement layer** (P2),
adapters resolve quirks through a single entry that also honours a
*runtime* policy layered on top of the build-time default — so a user
who doesn't trust a particular accommodation can dial it back without
recompiling, while the trusted set stays enforced.

```cpp
// The single entry adapters consume. Equivalent to make_quirks_for(...)
// plus the runtime base filter and any per-quirk overrides.
HostQuirks q = resolved_quirks(host_type, host_version);
```

**Precedence (highest first):**

1. **Per-quirk override** — `set_quirk_override("<field>", true|false)`.
   `true` keeps the host-populated value even if its tier is filtered
   out; `false` forces it back to the default (off). `clear_quirk_overrides()`
   resets them.
2. **API policy** — `set_host_quirk_policy(QuirkFilter{...})` sets the
   whole-policy filter programmatically; pass `std::nullopt` to clear.
3. **Environment** — `PULP_HOST_QUIRKS` (case-insensitive):
   `off` | `validated-only` | `all`. Parsed once per process; an
   unrecognized value is ignored with a one-time warning.
4. **Build-time default** — the `PULP_HOST_QUIRKS_DEFAULT_POLICY` CMake
   option (see above). When nothing else is set, behaviour is exactly
   the build-time default — i.e. P2 changes nothing by default.

All of these are **init-time** configuration: resolve once when the
adapter is constructed. None of the setters are real-time safe — never
call them from the audio thread.

`resolve_quirk_policy()` returns the effective base filter together with
its `QuirkPolicySource` (compile default / env / API) for diagnostics.

### Adapter consumption (enforcement status)

The format adapters cache `resolved_quirks(detect_host_info()...)` once at
init and gate accommodations on the flags. Flags wired so far:

- **`clamp_latency_to_nonneg`** (P3a) — VST3 / CLAP / AU v2 / AU v3 route
  latency reporting through `reported_latency_samples()`; a negative
  `latency_samples()` clamps to 0 by default and reports raw under
  `PULP_HOST_QUIRKS=off`.
- **`silence_unsupported_bus_arrangements`** (P3c) — VST3 `setBusArrangements`
  accepts an arrangement the processor can't natively support (instead of
  `kResultFalse`) and `process()` clamps the processor to its prepared
  channel count + silences the host's extra channels. `PULP_HOST_QUIRKS=off`
  preserves the original reject behavior. (VST3-scoped — the AU/CLAP adapters
  are declarative and don't reject arrangements.)
- **`synthesize_bypass_parameter`** (P3b) — when the plugin declares no
  Bypass param, VST3 + AU v3 inject an automatable one (`maybe_synthesize_bypass`
  in `quirk_apply.hpp`, reserved ID `kSynthesizedBypassParamId`); the
  adapters' existing bypass detection adopts it and `process()` honors the
  pass-through. `PULP_HOST_QUIRKS=off` synthesizes nothing. (CLAP + AU v2
  have no bypass process path yet — a synthesized param there would
  appear-but-do-nothing, so they're a separate follow-up.)

  > **State round-trip caveat:** the synthesized Bypass persists under its
  > reserved ID like any param. If a project is saved with synthesis active
  > (bypass engaged) and later loaded with `PULP_HOST_QUIRKS=off`, the param
  > isn't re-synthesized, so the persisted value is silently dropped on load
  > (the plugin reopens un-bypassed) — `StateStore` skips unknown IDs by
  > design. The reverse (saved off → loaded on) is benign: the synthesized
  > param keeps its safe default (not bypassed). Since the quirk is
  > `Validated` it survives the `validated-only` policy, so this only bites
  > when a user *explicitly* sets `PULP_HOST_QUIRKS=off` between save + load.

Remaining flags are wired incrementally, one PR per quirk, each with its own
validation — a flag is only meaningfully `Validated` once an adapter consumes
it under test.

### Inspecting the live policy: `pulp doctor`

```text
$ pulp doctor --host-quirks
Host quirks
  policy:        all  (compile default)
  detected host: REAPER 7.20
  enforced:      9 / 37 accommodations active
    • reaper_vst3_gesture_ordering  [Speculative]
    • ...
  override with PULP_HOST_QUIRKS=off|validated-only|all
  provenance + last-verified dates: core/format/host-quirks.json
```

The same section appears at the bottom of the default `pulp doctor`
output. `PULP_HOST_QUIRKS=off pulp doctor --host-quirks` shows the
policy switching to `off` with source `PULP_HOST_QUIRKS env`. Per-quirk
provenance — `source_type`, `evidence`, and `last_verified` dates — lives
in the machine-readable catalog `core/format/host-quirks.json`.

## Filing a new quirk

When a Pulp plugin breaks in a DAW you should follow the discovery
workflow documented in `planning/host-quirks-log.md` (Pulp's private
planning submodule):

1. Reproduce the bug + file a Pulp issue with the host, version, OS,
   format, and reproducer.
2. Read the host's documentation. Do **not** open the reference
   framework to copy.
3. Design a clean-room fix using only host docs + reproducer.
4. Add a new field to `HostQuirks` (and the matching `HostQuirksMeta`
   field tagged `Speculative` if you haven't bench-confirmed it yet,
   or `Validated` once you have a regression test) in
   `core/format/include/pulp/format/host_quirks.hpp`.
5. Wire it in the relevant per-host header (`apply_<host>()`) and the
   adapter that consumes it.
6. Add a Catch2 regression test alongside the existing
   `test/test_host_quirks.cpp` cases. Promote the meta tier from
   `Speculative` to `Validated` once you've added the in-DAW evidence
   row to `planning/host-quirks-log.md`.
7. Commit with the trailer:

   ```
   Reference-Lineage: cleanroom reproducer=#NNNN docs=<url>
   ```

## When to flip the build-time default

| Audience                                                                 | Suggested policy        |
|--------------------------------------------------------------------------|-------------------------|
| Most users — enforce bench-validated accommodations, evidence-gate the rest. | `validated_only` (default) |
| Plugin authors who trust Pulp's full curated catalog (incl. Speculative). | `all`                  |
| Compliance / diagnostic builds investigating whether a host or a Pulp accommodation is misbehaving. | `off` |

The default is `validated_only`: only quirks that have been bench-validated
(and the always-on cheap defenses, which are `Validated`) fire by default.
Because every quirk an adapter currently consumes is `Validated`, flipping
to this default changed no shipped behavior — it sets the contract that
future `Speculative` host quirks stay dormant until they earn `Validated`.
