# Copy-paste profile-authoring prompt

```text
Create a Pulp Sample Heritage profile for the sound or mechanism described at
the end of this prompt.

Use the current public Pulp Heritage profile-authoring skill, guide, CLI help,
and schema. Work only with data-only profile blocks; do not modify DSP source.
Choose a caller-owned folder or repository for the evidence bundle. Do not
assume access to a Pulp maintainer's private artifacts.

First describe the desired character neutrally. Research factual mechanism
claims with legally accessible primary or technical sources such as owner or
service manuals, schematics, patents, converter data sheets, standards, and
published papers. Do not inspect proprietary source, decompiled firmware,
leaked material, or another emulator's implementation. Record complete
citations, precise page/figure/section locators, access dates, use or license
basis, and whether each value is confirmed, probable, or test. Separate source
claims from your inferences.

Company, product, and model names remain their owners' property. Use a factual
name only in research provenance when necessary. Never put a mark in the
neutral profile ID or imply affiliation, endorsement, or an exact hardware or
serial-number match. Add a non-affiliation statement if the evidence bundle
names a reference product.

Deliver:
1. a neutral ID under neutral.* and canonical importable profile JSON;
2. a completed pulp.sample-heritage-evidence.v1 artifact manifest mapping every
   researched value to evidence and confidence;
3. the original authoring profile, canonical export, exact command log,
   validation and inspection reports, render reports, listening notes, and
   SHA-256 hashes in the caller-selected archive;
4. synthetic fixtures that isolate enabled blocks plus all-bypass or factor-one
   controls;
5. one disposable negative control made by changing profile_id to
   invalid-control, with proof that validation fails for the expected reason;
6. a short, level-matched listening recipe that describes character without
   claiming hardware identity;
7. explicit unresolved values and any mechanism the current blocks cannot
   represent.

Run these commands with the actual paths and fixtures:
  pulp audio heritage validate profile.json --json
  pulp audio heritage canonicalize profile.json --out canonical.json
  pulp audio heritage canonicalize canonical.json --out canonical-again.json
  cmp canonical.json canonical-again.json
  pulp audio heritage inspect canonical.json --json
  pulp audio heritage render canonical.json --fixture <impulse|sine|two-tone|WAV> --out <wav> --report <json>

Import and re-export the canonical profile and prove its digest is unchanged.
Require finite output, deterministic reruns when promised, correct declared
latency and streaming bounds, and no unexplained level or duration changes. If
the behavior is not expressible with an existing typed block, stop and report
the gap instead of silently approximating it.

Desired character or research target, with any legally usable measurements or
references:
<DESCRIBE TARGET HERE>
```
