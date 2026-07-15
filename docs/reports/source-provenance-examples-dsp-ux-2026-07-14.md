# Targeted source-provenance audit: examples, DSP, and UX — 2026-07-14

## Executive conclusion

No copied JUCE comment, copyright header, or exact lexical function was found
in the current Pulp examples, DSP, or UX source selected for this audit. One
DSP helper, `FastMath::tanh`, uses the same distinctive Padé coefficients and
nearly the same nested polynomial expression as JUCE's implementation. The
formula was publicly documented before JUCE added it, so this is not evidence
that JUCE was necessarily the source; however, Pulp does not record a neutral
source or derivation. It should be treated as provenance debt and replaced or
documented from a neutral mathematical reference.

The remaining high-scoring function candidates were short C++ idioms,
textbook vector operations, switches, or platform/API-mandated callbacks. The
remaining comment candidates were generic technical phrases and notation. No
other remediation candidate survived manual review.

This is a focused engineering diligence result, not a legal opinion. It cannot
prove that no infringement exists, and it does not compare Pulp against every
possible upstream repository.

## Scope and snapshots

- Pulp: current `origin/main` at
  `6e4b890534b9af734a62c78466b577b49bc9ec3e`.
- JUCE: local `master` at
  `3ba67d4585e9d1fbcdb26a877c7978608b1f802e`.
- Pulp roots:
  - `examples/`
  - `core/audio/`
  - `core/signal/`
  - `core/view/`
  - `core/render/`
  - `core/canvas/`
  - `core/scene/`
  - `core/native-components/`
  - `packages/pulp-react/`
  - `packages/pulp-web-player/`
- Coverage: 1,476 Pulp files and 4,424 JUCE files.
- Extracted Pulp/JUCE comments: 12,998 / 38,314.
- Extracted Pulp/JUCE C-family functions: 8,166 / 38,888.
- Extracted Pulp/JUCE JavaScript/TypeScript functions: 3,010 / 195.

The user deliberately limited the automated work to current-tree comment and
function comparison. Broad all-history scanning was excluded. Git history was
consulted only to adjudicate concrete candidates.

Known findings outside this scope remain in
`docs/reports/license-provenance-first-pass-2026-07-14.md`: the explicit
`NormalisableRange` transcription is in `test/`, and `AbstractFifo` is in
`core/runtime/`. This report does not supersede those findings.

## Method

### Comments

Source comments were grouped, stripped of comment syntax and URLs, lowercased,
and tokenized. The comparison checked:

1. exact normalized comment equality;
2. rare normalized six-word shingles; and
3. sequence and containment similarity for the resulting candidate pairs.

The manual review included the high-score tail and every direct JUCE reference
in the scoped Pulp source. License texts and non-source binary assets were not
part of this comparison.

### Functions

Tree-sitter C++, JavaScript, and TypeScript grammars extracted
C/C++/Objective-C++ and JavaScript/TypeScript function definitions. Script
functions were compared only with the JUCE script corpus, not across languages.
Two token representations were compared:

1. lexical tokens preserving identifiers and literals; and
2. normalized tokens replacing identifiers and literals while retaining
   syntax, operators, and control flow.

The comparison checked exact lexical and normalized hashes, rare normalized
seven-token shingles, sequence similarity, and normalized AST node-type
similarity. Scores were used only to rank manual review; they are not legal or
copying thresholds.

Raw output was intentionally left outside the repository at
`/private/tmp/pulp-source-provenance-20260714/current-tree-similarity.json`.
It is 646,952 bytes with SHA-256
`e400dfd407053240c1ebb85f941867c43645f17e60b11832fe275f8435703ced`.
The temporary comparison script has SHA-256
`bc1ac0088b017020f827b6d6bd42fa26882c8a7feacdcf17f7ba230c0f99cce1`.

## Results

### Comments: no remediation finding

- Exact normalized matches: **0**.
- Candidate pairs retained for ranking: 188.
- No JUCE or Raw Material Software copyright header was found in scope.
- Direct Pulp comments that name JUCE are independently worded descriptions of
  compatibility, behavior, or design reference. They are not copies of JUCE
  comments.

The strongest apparent comment similarities were non-expressive technical
phrases such as “given number of channels,” affine matrix labels `a b c d e f`,
normalized range notation, and the standard name “modified Bessel function of
the first kind.” These do not indicate copied prose.

Examples of direct JUCE references manually checked include musical typing,
anchored callouts, control painters, slider skew behavior, SVG fill behavior,
host parameter surfaces, and plugin embedding. None produced a matching JUCE
comment candidate. Some comments intentionally record behavioral prior art;
that is different from copying upstream prose.

Confidence: high against exact or lightly edited copied comments in the
current scoped tree.

### Functions: zero exact lexical matches

- Exact lexical function matches: **0**.
- Exact identifier/literal-normalized matches: 219 pairs representing 75
  distinct Pulp C-family functions.
- Every exact normalized Pulp function was 36 normalized tokens or shorter.
- C-family candidate pairs retained for manual ranking: 250.
- JavaScript/TypeScript exact lexical matches: **0**.
- JavaScript/TypeScript exact identifier/literal-normalized matches: **0**.
- JavaScript/TypeScript candidate pairs retained for manual ranking: 250.

The normalized exact matches were constructors, accessors, equality operators,
resets, callback setters, and similarly constrained short idioms. Identifier
normalization deliberately makes many unrelated one-line functions identical;
none retained a lexical match.

The high-score non-exact tail was dominated by:

- lowercase and hexadecimal-digit helpers;
- enum-to-string or enum-to-platform-value switches;
- COM `QueryInterface`/`Release` implementations;
- CoreAudio listener callback signatures;
- power-of-two tests;
- small vector operations; and
- code inside third-party projects vendored by JUCE, including CHOC, Oboe,
  HarfBuzz, VST3 SDK, LV2 SDK, and AudioUnitSDK.

These categories are conventional, API-dictated, or matches against a shared
third-party surface rather than JUCE-owned expression.

The script-function tail was similarly generic: event-listener registries in
test DOM shims, small array callbacks, state-map accessors, and matches against
minified Doxygen highlighting code. No Pulp script function had an exact
lexical or normalized match to a JUCE script function.

Confidence: high against exact function copying in the current scoped tree;
good, but not absolute, confidence against lightly edited copying.

### P2 — `FastMath::tanh` lacks neutral provenance

Status: remediation/documentation warranted; derivation from JUCE is not
established.

Pulp's implementation is at
`core/signal/include/pulp/signal/fast_math.hpp:31-40`. JUCE's is at
`modules/juce_dsp/maths/juce_FastMathApproximations.h:100-114` in the named
checkout. Both compute `x2`, then use the same distinctive coefficients
`135135`, `17325`, `378`, `62370`, `3150`, and `28` in nearly the same nested
numerator/denominator structure. Pulp adds explicit saturation outside
`[-4, 4]`, uses float literals, and reverses the final `28 * x2` operand order.

Automated ranking reported normalized token sequence similarity 0.7785, AST
node-type sequence similarity 0.7911, and rare seven-token shingle containment
0.3265. Those whole-function scores are diluted by Pulp's additional clamp;
the core three-statement rational expression is substantially closer.

The Pulp function entered in initial-release commit
`128963148254baca63322422b235b05534c25a65` on 2026-04-06 without a source
citation. JUCE's equivalent entered in
`244a944857823766e6130d15be8f8ea4f1b83ddc` on 2017-07-27.

The coefficients are not unique to JUCE. The same continued-fraction
approximation was publicly posted in DSP discussions before JUCE's
implementation, including a 2012 KVR post and a 2016 Spin Semiconductor post:

- <https://www.kvraudio.com/forum/viewtopic.php?p=4917534>
- <https://www.spinsemi.com/forum/viewtopic.php?t=628>

That prior publication and the mathematical nature of the expression reduce
the evidence for JUCE-specific derivation. The lack of recorded provenance
still prevents a confident clean-room conclusion.

Recommended remediation:

1. Re-derive the rational polynomial from a cited neutral mathematical source
   and express it independently, or replace the optional approximation with
   `std::tanh` if the performance tradeoff is no longer useful.
2. Retain tests against the neutral mathematical specification rather than
   JUCE output.
3. Record the source and independent review in the commit message.
4. Have counsel decide whether the compact mathematical expression requires
   any further action; this report makes no infringement conclusion.

Confidence: high that the core expression is very close; low-to-medium that
JUCE, rather than the pre-existing public formula, was Pulp's source.

### Reviewed low-concern candidate — `Vector3D`

`core/view/include/pulp/view/animator_set.hpp:258-274` and JUCE's
`juce_Vector3D.h` share the conventional `Vector3D` name and textbook
component-wise addition, subtraction, scalar multiplication, dot product,
cross product, and Euclidean length formulas.

Pulp's class was introduced by
`ef5e75e5f95dc1a4de4384631ef20ff30abc7ec8` without a JUCE reference. Its API
and normalization behavior differ, and the matching expressions are tightly
constrained by the mathematical definitions. No copied comment accompanied
the implementation.

Classification: conventional mathematical implementation; no remediation.

### Other directed candidates

- `FftT::forward_real_fallback` and JUCE both copy real samples into complex
  storage with zero imaginary parts before invoking an FFT. The surrounding
  algorithms and storage contracts differ; this is the required operation.
- CoreAudio listener functions share Apple callback signatures and extract a
  client pointer. This is API-mandated boilerplate.
- Windows COM implementations share `QueryInterface`, reference-count, and
  `Release` shapes. This is API-mandated boilerplate.
- Large switch candidates map unrelated Pulp and JUCE enums to unrelated
  platform values. Identifier/literal normalization intentionally exaggerates
  their similarity.

No remediation was identified for these candidates.

## Existing prevention controls and their limits

Pulp has real license and clean-room controls, but they do **not** currently
protect ordinary examples, DSP, and UX edits from function/comment copying.

### Controls that exist

- `.github/workflows/license-audit.yml` runs the dependency audit and
  `tools/audit.py` on pull requests.
- `tools/scripts/check_import_provenance.py` validates provenance markers and
  scans generated importer output for framework-source marker tokens.
- `tools/import/known-frameworks.json` supplies JUCE/iPlug2/VST3 detection
  markers as data.
- `tools/scripts/gates.sh` can invoke the import-provenance check.
- `CLAUDE.md` requires a `Reference-Lineage` clean-room trailer for the narrow
  host-quirks surface.

### Coverage gaps

- `tools/audit.py` checks license files and forbidden vendor SDK filenames; it
  does not compare source functions or comments.
- The import-provenance gate is opt-in through
  `PULP_IMPORT_PROVENANCE_DIRS` and is a no-op on normal Pulp pushes.
- Its content denylist scans only files marked generated/stub and detects
  framework marker strings, not copied expressions with renamed identifiers.
- The `Reference-Lineage` trailer applies only to host quirks, not examples,
  DSP, canvas, rendering, or views.

Recommended hardening:

1. Keep the importer clean-room control separate; its current purpose is
   valid.
2. Add a private scheduled or diff-scoped provenance job that compares changed
   Pulp-owned functions/comments against a pinned approved upstream corpus and
   fails only on new exact lexical matches or explicitly reviewed thresholds.
3. Require a lineage record when a Pulp-owned source comment names a framework
   as implementation prior art, while exempting interoperability and migration
   documentation.
4. Baseline and allowlist reviewed conventional/API matches so the control is
   actionable rather than noisy.

## Validation

- The final corrected C-family plus JavaScript/TypeScript comparison completed
  in 149.99 seconds.
- Dependency strict audit: passed.
- `tools/audit.py --`: passed.
- `tools/import/test_project_import_ir_schema.py`: 13 tests passed.
- A requested `tools.scripts.test_check_import_provenance` unittest module does
  not exist; the importer-provenance behavior is instead covered in the C++ CLI
  test surfaces. No claim of running that absent module is made.
