# License and provenance first-pass audit — 2026-07-14

## Executive conclusion

No broad JUCE clone pattern or wholesale framework inclusion was found in
Pulp's current `origin/main` tree or its main-branch history. A bounded
semantic/history pass did find one explicit JUCE `NormalisableRange`
transcription that warrants remediation and one medium-confidence
`AbstractFifo` lineage concern that warrants clean-room review.

The license inventory also found one active redistribution compliance problem
in the published web-player npm package and two lower-severity
inventory/repository-hygiene gaps. All three inventory findings now have
source-side remediations in the audit worktree; the already-published npm
artifact remains affected until a corrected version is explicitly published.

This is a focused engineering diligence pass, not a legal opinion. Its purpose
is to identify obvious, high-signal problems and record the likely remediation
before deeper diligence is considered.

## Scope and snapshot

- Pulp source: freshly fetched `origin/main` at
  `759669953c814b379a3f465d0b11d527452236c9`.
- Isolated worktree:
  `/Users/danielraffel/Code/pulp-license-diligence-20260714`.
- Candidate upstream: local JUCE `master` at
  `3ba67d4585e9d1fbcdb26a877c7978608b1f802e`.
- ScanCode Toolkit: stable `v32.5.0` from a detached worktree of the local
  `/Users/danielraffel/Code/scancode-toolkit` checkout.
- ScanCode coverage: 6,964 files, zero scan errors; license, copyright,
  package, generated-code, file-info, summary, tallies, clarity, and review
  diagnostics enabled.
- Raw machine output:
  `/private/tmp/pulp-license-diligence-20260714/scancode.json` (not suitable
  for committing because it is 11,365,341 bytes and contains extensive matched
  text). SHA-256:
  `a450a44a2b689f420f920bdc12c649842b34c2f5b76f60f9b6d95e3549901e4b`.

Excluded from this first pass: built release archives/installers, private
sibling repositories, and registry packages other than the directly relevant
published web-player artifact. A bounded function-level semantic comparison
against the specifically named JUCE upstream was added after the initial
deterministic pass; it was not a broad internet-scale similarity search.

## Findings

### P1 — Published web-player package omits required license payloads

Status: source remediation implemented; the live registry artifact remains
affected. The omission was confirmed in both the original local
`npm pack --dry-run` payload and `@danielraffel/web-player@0.2.1` (registry
metadata modified 2026-07-13).

The package publishes Pulp source under an MIT declaration and redistributes:

- `src/theme/Inter-Regular.ttf` — ScanCode identifies SIL OFL 1.1 and
  copyright 2016 The Inter Project.
- `src/theme/inter.woff2` — a second Inter font artifact.

The 0.2.1 tarball contains neither the Pulp MIT license text nor an Inter OFL
license/third-party notice. The repository-level `LICENSE.md` and `NOTICE.md`
are not included when npm packs the package subdirectory. The dependency
manifest also tracks only `external/fonts/Inter-Regular.ttf`, not the two font
copies shipped by this npm package.

Why it matters: downstream recipients do not receive the notices that accompany
the redistributed code and font. This is a real shipped-artifact compliance
gap, not a ScanCode keyword false positive.

Implemented remediation:

1. Added a package-local MIT `LICENSE` file.
2. Added `THIRD_PARTY_NOTICES.md` with the Inter copyright and verbatim full
   upstream SIL OFL 1.1 text.
3. Prepared package version 0.2.2 and made `npm test` parse the dry-run tarball
   manifest to require both legal files and both font artifacts.
4. Extended the publish script's independent `MUST_SHIP` gate to require the
   same files.
5. Added both packaged font paths and hashes to the Inter inventory entry and
   completed the root Inter OFL text, which had itself ended after condition 3.

Remaining external action: publish 0.2.2 and optionally deprecate 0.2.1 with a
message directing users to the corrected version. No registry mutation was
performed by this audit.

Confidence: high.

### P2 — Gradle wrapper was redistributed but absent from dependency inventory

Status: remediated in the audit worktree.

Pulp tracks `android/gradlew`, `android/gradlew.bat`, and
`android/gradle/wrapper/gradle-wrapper.jar`. The jar identifies itself as the
Gradle Wrapper under Apache-2.0 and contains `META-INF/LICENSE`; the scripts
also carry Apache-2.0 notices. The wrapper is not represented in
`tools/deps/manifest.json`, `DEPENDENCIES.md`, or the root `NOTICE.md`.

This is not an incompatible-license problem. It is an inventory/attribution
coverage gap and explains why the existing strict dependency audit passed even
though a redistributed third-party binary was outside its manifest.

Implemented remediation:

1. Added Gradle Wrapper 8.11 to the machine-readable manifest and all three
   attribution surfaces, including the jar SHA-256
   `b3a875ddc1f044746e1b1a55f645584505f4a10438c1afea9f15e92a7c42ec13`.
2. Recorded the verified upstream `v8.11.0` tag and the JAR's embedded
   Apache-2.0 metadata/license.
3. Added a focused strict-audit scanner and unit tests so a committed Gradle
   wrapper JAR without a corresponding manifest entry fails compliance checks.

Confidence: high.

### P2 — Generated Gradle problems report is committed

Status: remediated in the audit worktree.

`android/build/reports/problems/problems-report.html` is a 133,900-byte Gradle
generated report committed under a build-output directory. It embeds Gradle's
configuration-cache report application and normalize.css v7.0.0 (MIT), while
`android/.gitignore` currently ignores only `local.properties`.

This does not appear malicious or copyleft, but it is an unnecessary generated
third-party payload, creates attribution noise, and contains stale build
diagnostics. It should not be source-controlled.

Implemented remediation:

1. Removed the tracked report.
2. Ignored `android/.gradle/` and every `android/**/build/` output directory.
3. Recorded the redistribution/output distinction in the repository's Android
   maintenance guidance.

Confidence: high.

## JUCE provenance result

No broad clone pattern or wholesale JUCE inclusion was found. Two bounded
items merit follow-up because source comments and commit history directly name
JUCE; those facts are stronger provenance signals than a similarity score.

### P1 — Explicit JUCE `NormalisableRange` transcription in a test

Status: confirmed provenance debt; not remediated in this worktree.

The most direct evidence is in `test/test_normalisable_range.cpp:203-241`:
the test calls its helper an "Independent transcription of JUCE's
NormalisableRange<double>" and says it is kept "structurally parallel to the
JUCE source." The test asserts production equality against that helper at
lines 245-275.

Related implementations:

- Pulp production: `core/state/include/pulp/state/parameter.hpp:200-271`, with
  normalization at lines 232-243, inverse mapping at 247-263, and the centre
  skew formula at 217-223.
- JUCE reference: `modules/juce_core/maths/juce_NormalisableRange.h:136-196`
  in the named checkout, with centre-skew math at 210-217.
- Production entered through component commit `727cf74615394a601365108a25d1d1a8453a1c7d`
  and rollup commit `e624fcdfea1791051ca1628ed36f9198e96803da`.
- The explicit helper was added by `b4fdcdde2b75b3a0d9f0c4cbcb7889792ca6f8c6`;
  its commit message also calls it a JUCE transcription.

This is not an automatic infringement conclusion: the relevant behavior is
compact mathematics, and Pulp restructures some expressions. It is still a
high-confidence derivation signal because the repository expressly records
transcription and structural parallelism. The inspected JUCE header carries
Raw Material Software copyright and JUCE commercial/AGPL terms.

Recommended remediation:

1. Remove the transcribed in-tree reference helper.
2. Replace it with independently specified golden vectors or property tests
   that do not retain JUCE source expression.
3. Record the neutral functional specification, author, review process, and
   evidence that the replacement was produced without consulting JUCE source.
4. Have counsel decide whether the compact production implementation needs the
   same clean-room treatment and whether a relevant commercial JUCE license
   covered the work/source version. Replacing only the test should not be
   assumed to settle that question.

Confidence: high that the test helper was transcribed; medium-high that the
production behavior was intentionally implemented from the same named
reference; no legal conclusion made.

### P2 — `AbstractFifo` explicitly mirrors JUCE

Status: provenance/clean-room review warranted; not a confirmed copy and not
remediated in this worktree.

- Pulp source `core/runtime/include/pulp/runtime/abstract_fifo.hpp:25` says
  usage mirrors JUCE; the implementation is at lines 35-174.
- Introducing commit `df6191e390b42ddb685241660eed7a5299ca373c` says the
  class mirrors JUCE, names the paired prepare/finish API, and calls out the
  same one-slot sentinel convention.
- JUCE reference implementation is
  `modules/juce_core/containers/juce_AbstractFifo.cpp:38-141`.

The implementations are not textually close. Directed normalized sequence
similarity was 0.401-0.440 for the prepare/read/finish functions, and rare
seven-token shingle containment was about 8% for the prepare functions. The
algorithm is also a conventional SPSC circular-buffer idiom, so common control
flow is expected.

Recommended remediation:

1. Treat this as a provenance-documentation and clean-room-review item, not a
   confirmed license violation.
2. For conservative diligence, reimplement from a neutral SPSC specification
   or clearly permissive reference and retain the design/review record.
3. Remove unnecessary "JUCE mirror" language or rename the API after the
   neutral provenance is established.
4. Have counsel decide whether the existing expression is sufficiently
   independent.

Confidence: medium lineage concern; low confidence of protectable expression
copying.

### Comparison evidence and lower-concern candidates

Deterministic evidence gathered before the semantic pass:

- No current non-empty Pulp file has the same Git blob as a current JUCE file.
- Comparing the complete `origin/main` ancestry with JUCE's `master` ancestry
  found only two shared historical blobs: an empty generic Apple entitlement
  plist and a generic Apache-2.0 license text. Neither is JUCE framework code.
- No JUCE, Raw Material Software, ROLI, or JUCE Ltd copyright header was found
  in Pulp-owned source or in relevant main-branch history searches.
- A normalized five-line code-shingle comparison built 554,180 unique JUCE
  shingles and found zero candidate pairs in Pulp-owned source. Whitespace and
  comments were ignored.
- Direct current-tree JUCE references are primarily documentation,
  interoperability, migration guidance, tests/fixtures, or explicit
  clean-room policy—not vendored JUCE implementation files.

The bounded semantic pass compared 10,769 Pulp and 18,754 JUCE C-family
function bodies. It stripped comments/literals, alpha-renamed identifiers,
indexed rare normalized seven-token shingles, ranked 19,656 threshold
candidates, manually reviewed the high-score tail, and then ran directed
comparisons over files whose comments/history named JUCE.

Tree-sitter C++ was then installed only in a temporary environment to validate
the named candidates with real syntax trees. After identifiers and literals
were normalized, the explicitly transcribed test helpers had AST node-type
sequence similarity of 0.591 (`convert_to_0to1`) and 0.652
(`convert_from_0to1`) to the named JUCE methods. Production range methods were
0.366-0.526, and the centre helper was 0.429. `AbstractFifo` method pairs were
0.619-0.694. These figures are ranking evidence, not legal thresholds; the
source/history admissions remain the decisive reason the two items are logged.

Broad high matches were generic switches, `std::tolower` helpers, platform API
boilerplate, or matches against third-party code vendored inside JUCE (such as
CHOC, Oboe, HarfBuzz, and Box2D). Directed review found no semantic-similarity
remediation item for `FileSearchPath`, `AnchoredCallout`, musical typing,
`ExtensionsVisitor`, iOS orientation mapping, lowercase helpers, or CoreAudio
listener teardown; their implementations were materially different,
conventional, or API-dictated.

Confidence is high for ruling out literal copying and lightly edited contiguous
copying across the broader tree. The two explicitly named lineage items above
remain because authorship/history evidence changes the diligence threshold.

## Review of the proposed larger tool stack

For the stated goal—find obvious problems first—the larger stack is not needed
yet.

| Tool | Value now | When it becomes worthwhile |
|---|---|---|
| ScanCode Toolkit | High; use now | License texts, copyrights, package clues, vendored files |
| ORT | Medium; defer | Resolved dependency graphs, policy evaluation, SBOM and multi-ecosystem package diligence |
| Tree-sitter | Medium for named concerns | Candidate-based function extraction and AST comparison after a source-lineage concern exists; this pass used a broad normalized lexical screen and temporary Tree-sitter C++ validation for the named candidates |
| CodeQL | Separate track | Security and code-quality diligence, not primary license/provenance detection |
| Semgrep | Separate track | Enforcing known repository patterns; weak for discovering code provenance |
| Vector search | Defer | Broad semantic candidate discovery across many upstream repositories; requires careful false-positive review |
| LLM reasoning | Useful as reviewer | Classification and remediation after deterministic evidence, not as the source matcher |

A sensible second pass before an acquisition or major public release would be:

1. Scan the actual SDK, npm, application, plugin, and installer artifacts.
2. Use ORT or equivalent ecosystem-native resolution to evaluate transitive
   dependencies and produce an SBOM/policy report.
3. Add deterministic package-payload and unmanifested-binary gates.
4. Use AST/semantic comparison only for named upstream repositories or files
   where authorship/history provides a concrete reason for concern. This audit
   applied that rule to JUCE and found the two follow-up items above.

## Raw ScanCode triage notes

ScanCode reports GPL/LGPL/AGPL/proprietary strings in several Pulp-owned files,
but manual review showed the high-signal current-source cases are explicit
descriptions of external boundaries, test fixtures, or policy data—for example
LAME, Avahi, libnotify, system JavaScriptCore, aubio, and AQUA-Tk. They are not
license headers asserting that Pulp source is under those licenses.

The original `python3 tools/deps/audit.py --strict --format markdown` check
passed despite these gaps. The remediated version still passes, now recognizes
the redistributed Gradle wrapper as an independent declaration source, and has
a negative test proving removal of its manifest entry is detected. The npm
package has a separate artifact-level gate because repository-manifest
validation alone cannot prove what a registry tarball contains.

## Remediation validation

- `python3 -m unittest tools.deps.test_audit_extra` — 16 tests pass.
- `python3 tools/deps/audit.py --strict --format markdown` — passes.
- `npm --prefix packages/pulp-web-player test` — passes, including the packed
  legal payload test.
- `npm pack --dry-run --json` from the package directory — reports version
  0.2.2 and contains `LICENSE`, `THIRD_PARTY_NOTICES.md`,
  `src/theme/Inter-Regular.ttf`, and `src/theme/inter.woff2`.
- The package's Inter license body is byte-for-byte identical to upstream
  `rsms/inter` `LICENSE.txt` at the time of validation.
- Temporary Tree-sitter C++ parsing completed for the named JUCE candidates;
  no parser dependency or generated output was added to Pulp.
- Android skill-sync tests and the generated skill catalog check pass. The
  broader `test_skill_path_map.py` retains an unrelated baseline failure for
  two absent `planning/2026-06-11-pulp-package-format-proposal.md` mappings
  under the `kits` and `content` skills.
- `git diff --check` — passes.
