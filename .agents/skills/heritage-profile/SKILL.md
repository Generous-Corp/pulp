---
name: heritage-profile
description: Research, author, validate, render, and archive data-only Pulp Sample Heritage profiles. Use when creating or revising a sampler character profile, importing or exporting Heritage JSON, proving profile behavior with analytic and listening fixtures, recording evidence and trademark-safe provenance, or preparing a caller-owned profile artifact bundle.
---

# Heritage Profile Authoring

Create evidence-backed sampler character as portable profile data. Keep the
runtime profile neutral and the research record separate. Never require access
to a private artifact repository.

Before authoring, read the current Pulp Sample Heritage guide and run:

```bash
pulp audio heritage --help
```

Read [references/profile-format.md](references/profile-format.md) whenever
selecting blocks, importing/exporting, or handling a schema version. Copy
[assets/profile-template.json](assets/profile-template.json) as the starting
profile. Copy [assets/artifact-manifest-template.json](assets/artifact-manifest-template.json)
into the caller-selected artifact directory or repository.

## Workflow

1. **Choose caller-owned locations.** Ask for or infer a working directory for
   the profile and a separate archive directory or repository for research,
   prompts, reports, rendered audio, and hashes. Do not default to any Pulp
   maintainer's private repository. A private overlay skill may supply these
   paths, but the workflow must also work without that overlay.

2. **Describe the target neutrally.** Write audible and mechanical goals such
   as “gritty low-rate percussion with clock-linked pitch and a dark output”
   before researching a named reference. Use a `neutral.*` profile ID composed
   only of lowercase ASCII letters, digits, dots, and hyphens. Do not put a
   company, product, model, or other mark in the ID.

3. **Research cleanly.** Prefer legally accessible primary or technical
   sources: owner and service manuals, schematics, patents, converter data
   sheets, standards, and published technical papers. A personally controlled
   capture is optional evidence when its provenance and right to use are
   recorded. Do not inspect proprietary source, decompiled firmware, leaked
   material, or another emulator's implementation. Do not copy source material
   wholesale into the archive.

4. **Record evidence before choosing values.** For every researched value,
   record the exact profile field, value or range, source kind, full citation,
   page/figure/section locator, access date, use or license basis, and confidence
   as `confirmed`, `probable`, or `test`. Keep inference distinct from a source's
   explicit claim. Record unresolved values as `test`; do not invent precision.

5. **Respect names and marks.** Company, product, and model names remain their
   owners' property. Use them only when factually necessary inside source
   citations or research provenance. Never use them in neutral IDs, imply
   affiliation or endorsement, or market a profile as an exact product/serial
   match. Include a non-affiliation statement in any artifact bundle that names
   a reference product.

6. **Author typed data only.** Copy the neutral template, change the ID, and add
   only blocks supported by the current schema. Preserve domain and block order.
   Do not modify DSP source to force a profile through. If a mechanism is not
   expressible, stop and report the missing mechanism and evidence instead of
   silently substituting an approximation.

7. **Validate and canonicalize.** Run the command family below, adjusting only
   paths and render fixture arguments:

   ```bash
   pulp audio heritage validate profile.json --json
   pulp audio heritage canonicalize profile.json --out canonical.json
   pulp audio heritage canonicalize canonical.json --out canonical-again.json
   cmp canonical.json canonical-again.json
   pulp audio heritage validate canonical.json --json
   pulp audio heritage inspect canonical.json --json
   pulp audio heritage render canonical.json --fixture <impulse|sine|two-tone|WAV> --out <wav> --report <json>
   ```

   Require canonicalization to be byte-idempotent. Import the canonical file,
   export it again, and require the same profile digest. Preserve the original
   authoring input beside the canonical export.

8. **Prove the validator can fail.** Copy the profile to a disposable negative
   control, change `profile_id` to `invalid-control` (missing the required
   `neutral.` prefix), and run `validate`. Require a nonzero exit and an exact
   profile-ID diagnostic. Record that result, remove the invalid copy, then
   re-run validation on the untouched canonical profile. A check that has only
   ever passed is not sufficient evidence.

9. **Render analytic fixtures.** Use the built-in `impulse`, `sine`, and
   `two-tone` fixtures, or an exact-profile-rate mono WAV, to isolate each
   enabled block: silence for noise/idle gating, impulse or step for filters and
   hold behavior, sine sweeps or tones for converters and nonlinear stages, and
   periodic/transient material for clock, pitch, and stretch. Also render an
   all-bypass or factor-one control. Require finite output, deterministic reruns
   when seeds promise determinism, declared latency and streaming bounds, and
   no unexplained level or duration changes. Use `--frames` and `--block-size`
   when a proof needs explicit duration or partitioning. Save commands, reports,
   WAV hashes, and observations. Treat record-commit as the separately reported
   offline stage; do not assume it was silently composed into a cross-rate live
   render.

10. **Listen without overclaiming.** Compare level-matched rendered controls
    and the candidate on representative percussion, tonal, transient, and
    sustained material as relevant. Randomize presentation when practical.
    Record observations and open questions, not claims of hardware identity.
    Listening complements analytic evidence; it does not replace it.

11. **Archive reproducibly.** Save the desired-character brief, the exact agent
    prompt, research notes or links, evidence manifest, original and canonical
    profiles, validation/inspect/render reports, listening notes, and SHA-256
    hashes in the caller-selected archive. Exclude copyrighted source copies,
    secrets, private machine identifiers, and unrelated private evidence. Link
    the archive to the resulting profile only when the caller wants that link.

## Deliverable gate

Do not call a profile complete until all of these are present:

- a neutral canonical profile that validates and re-exports without digest
  drift;
- a passing validation report and an intentionally failing negative-control
  report;
- evidence for every researched value with a locator and confidence;
- analytic render reports, audio hashes, and a short listening record;
- a completed artifact manifest and reproducible command log;
- clear disclosure of unresolved `test` values and any missing mechanism.

No named Pulp Heritage profile is implied to ship merely because this workflow
or its templates exist.

## The typed bus path cannot absorb an oversized block

`PulpSamplerProcessor::render_output_segment` has two shapes.
`render_active_voices` chunks by the block width declared at prepare, so it
tolerates a host handing it more frames than it asked for. The typed-heritage
bus path cannot: it passes `process_bus` a single activity span over the whole
segment, and `bus_voice_activity_` was sized once at prepare. Indexing that
buffer by the block's actual frame count writes past the end.

The overrun is silent. It corrupts allocator metadata rather than faulting, so
it surfaces later and somewhere else — `mutex lock failed: Invalid argument`,
or an abort raised from inside `libsystem_malloc`. Running the test binary
unfiltered can even print "All tests passed", because by then the heap has
enough slack for the stray bytes to land on nothing live; only one-process-per-
case (`ctest`, via `catch_discover_tests`) reliably smashes something. **A green
unfiltered run is not evidence.** Reach for Guard Malloc
(`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`) — it names the overrun and
faults at the write.

So when adding a heritage fixture, prepare it at the largest single block it
will be driven with. A fixture prepared at 512 and processed with 2048 is out
of contract, and the bus path now refuses that segment rather than corrupting
the heap.

## A wall-clock budget under a sanitizer measures the sanitizer

The heritage shipping gates assert CPU budgets — a RATIO of chain time to a
baseline. Sanitizer instrumentation does not tax the two halves equally (it
costs the chain's extra work more than the baseline's), so the ratio drifts on
an instrumented build and runner load, not the code, decides the verdict. The
same gate passed on one PR and failed on another within an hour.

`#if defined(NDEBUG)` does **not** protect you here: sanitizer builds are
`RelWithDebInfo`, so NDEBUG is defined and the budget runs instrumented anyway.

The pattern (established in `timeline_tests.cmake`, now also in
`sampler_runtime_tests.cmake`): detect the sanitizer in CMake and pass a
compile definition the test can see.

```cmake
set(PULP_HERITAGE_GATES_SANITIZED OFF)
if(PULP_SANITIZER OR CMAKE_CXX_FLAGS MATCHES "(^|[ ;])-fsanitize")
    set(PULP_HERITAGE_GATES_SANITIZED ON)
endif()
# then, on the target:
#   $<$<BOOL:${PULP_HERITAGE_GATES_SANITIZED}>:PULP_TEST_WITH_SANITIZER=1>
```

```cpp
#if defined(NDEBUG) && !defined(PULP_TEST_WITH_SANITIZER)
    REQUIRE(measured_ratio <= budget);
#else
    SUCCEED("… enforced by the uninstrumented Release configuration");
#endif
```

**Guard the timing half only, never the whole test.** Excluding the test from
sanitizer lanes by name or label drops the entire binary, losing genuine
memory/UB coverage of these paths. Everything exact — validity, capacity, and
frame bounds — should still run under instrumentation.

When you add or change a budget assertion, verify **both** directions by
preprocessing with each build's real flags from `compile_commands.json`: the
budget code must be present in a plain Release build and absent under the
sanitizer. A guard that silently disabled the budget everywhere would look just
as green as a correct one.

## Copy-paste prompt

For a ready-to-send prompt that asks another agent to perform this workflow,
read and copy [references/agent-prompt.md](references/agent-prompt.md). Replace
the final placeholder with the requested sound, mechanism, and any legally
usable references or measurements.
