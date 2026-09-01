# Authoring capsule (OAAC v1)

`pulp::authoring_capsule` is an optional, product-agnostic substrate for
portable authoring capsules: a bounded container plus a canonical manifest that
describes exactly what is inside, what it is compatible with, what it depends
on, and what rights attach to each component.

The layer knows nothing about any specific product. It owns admission,
canonicalization, closure, and safety. A *profile* — registered by a consumer —
owns the meaning of the payload.

**Status:** provisional. `.forge` is the first product-facing extension; the
format identifier is vendor-neutral so other products can register profiles.

## Scope

Owned by this module:

- bounded, deterministic archive reading and writing;
- canonical envelope parsing and serialization;
- exact file-role, media-type, byte-count, and SHA-256 closure;
- safe path normalization, collision policy, archive budgets, and hostile-member
  rejection;
- compatibility, dependency, capability, and completeness result types;
- non-executing preview data;
- optional signature/attestation adapter interfaces;
- extraction into owner-private staging and atomic no-replace publication;
- a profile codec/validator registry interface.

Explicitly **not** owned: any product schema, marketplace client behavior,
network transport, UI, or code execution of any kind.

## Container

A capsule is a ZIP-compatible archive. Its first member is a canonical UTF-8
JSON manifest at the fixed path `capsule.json`. The manifest is the only
authority: the file extension, any marketplace category, the title, and the
tags are descriptive and must never route runtime behavior.

### Archive budgets (v1)

Admission fails closed when any limit is exceeded.

| Limit | Value |
|---|---|
| Members | 20 000 |
| Compressed archive bytes | 2 GiB |
| Total expanded bytes | 8 GiB |
| Single member expanded bytes | 2 GiB |
| Expansion ratio, per member and overall | 200:1 |
| Manifest member bytes | 16 MiB |
| Path bytes (UTF-8, normalized) | 1024 |
| Path depth | 32 |

Compression methods are limited to *store* and *deflate*. Encrypted members,
symlinks, hardlinks, devices, and other special modes are rejected. The
manifest must be the first member. Every archive member must appear in
`files[]`, and every `files[]` row must appear in the archive: an undeclared
member and a declared-but-absent member are both closure violations.

### Path rules

Paths are NFC-normalized, forward-slash separated, and relative. Rejected:
absolute paths, drive letters, UNC prefixes, any `.` or `..` component, leading
or trailing slashes, empty components, NUL and C0 control characters, and
trailing dots or spaces. Two paths that differ only by ASCII case, by Unicode
normalization form, or by a confusable-character substitution are a collision
and are rejected.

## Manifest

```json
{
  "format": "org.pulp.audio-authoring-capsule",
  "format_version": 1,
  "profile": "com.generous.forge.instrument",
  "profile_version": 1,
  "product": "forge-instrument",
  "authoring_kind": "instrument",
  "subtypes": ["sampler"],
  "topology": {},
  "required_capabilities": ["pcm-sample-bank"],
  "project_id": "…",
  "revision_id": "sha256:…",
  "parent_revision": "sha256:…",
  "reproducibility": "best-effort",
  "compatibility": {},
  "dependencies": [],
  "files": [],
  "title": "…",
  "created_at": "2026-08-29T00:00:00Z",
  "provenance": {},
  "attestations": [],
  "distribution": {}
}
```

`format` is the constant `org.pulp.audio-authoring-capsule`. `format_version`
and `profile_version` advance independently.

An unknown **required** role, profile, or capability fails closed. An unknown
**optional** descriptive key round-trips unchanged: a reader must preserve it
and a re-export must reproduce it byte-for-byte in canonical form.

### `reproducibility`

| Value | Meaning |
|---|---|
| `reproducible` | Declared inputs plus pinned dependencies regenerate the canonical result, and receipts prove it. |
| `best-effort` | Source and history are present, but a model, tool, or environment cannot be replayed exactly. |
| `frozen-output-only` | Only playable derived artifacts are present. |

### `completeness` (derived, never authored)

`self_contained`, `resolvable`, `partial`, `play_only`. It is computed from the
component rows; a capsule that declares it is ignored.

## Component policy

Every row in `files[]` and `dependencies[]` carries its own policy. There is no
project-wide `open_source` or `locked` authority.

There is no separate `components` array. A component *is* a `files[]` or
`dependencies[]` row: one authority, so a policy cannot disagree with the row
it describes.

Row fields, with the policy fields nested under a single `policy` object:

| Field | Values |
|---|---|
| `role` | profile-defined semantic role, e.g. `dsp.source` |
| `path` (files) / `id` (dependencies) | safe relative path, or content identity |
| `sha256`, `bytes`, `media_type` | exact, required |
| `executable_data` | `true` when the bytes are code the product could run |
| `provider` (dependencies only) | allowlisted HTTPS provider or library locator |
| `policy.canonicality` | `canonical-input`, `derived-output`, `preview`, `receipt` |
| `policy.source_availability` | `included`, `external`, `local-only`, `omitted` |
| `policy.editability` | `editable`, `opaque` |
| `policy.disclosure` | `public`, `recipient-scoped`, `private`, `redacted`, `not_recorded` |
| `policy.redistribution` | `allowed`, `restricted`, `unknown` (defaults to `unknown`) |
| `policy.license_expression` | SPDX expression or `LicenseRef-…` |
| `policy.license_notice_sha256` | digest of the included notice, when one is required |
| `policy.creator`, `policy.source_uri`, `policy.attribution_required` | provenance facts |
| `policy.required_for` | any of `play`, `rebuild`, `remix`, `publish` |

A `files[]` row must declare `source_availability: included`. A row means the
bytes are in this archive, so `external`, `local-only`, and `omitted` are
contradictions and are rejected as `manifest_invalid` rather than admitted as a
component nothing can ever resolve. Anything not in the archive is a
`dependencies[]` row, which is where `provider` lives.

`unknown` never becomes permissive by omission or by projection. A hash
establishes identity and integrity; it grants no right to copy, remix, publish,
or sell.

The substrate enforces that in the type rather than stating it in a comment.
`Redistribution` default-, value-, and zero-initializes to `unknown`, and the
granted state is reachable only by naming `Redistribution::granted()`, so a row
cannot acquire a grant from an aggregate initializer, a forgotten field, or an
enumerator that happens to sit at zero. The wire tokens are unchanged:
`allowed`, `restricted`, `unknown`. The rule exists because a grant is a claim
about someone else's work, and a permissive value that costs nothing to reach
is the one a writer reaches for without deciding anything.

### Rights facts a preview reports

A preview reports which components block a self-contained redistributable
claim. A blocker is named by a typed reference: kind `file` carrying the
archive path, or kind `dependency` carrying the dependency's `id`. The two row
kinds are identified in different namespaces and a dependency has no path, so a
path-only list would be structurally empty for a profile whose components are
entirely dependencies, which is the case where the list has the most to say.
The list is sorted by kind then identifier and deduplicated, so two previews of
the same capsule agree.

## Canonicalization and revision identity

Canonical JSON: UTF-8 without BOM, LF line endings, object keys sorted by
Unicode code point, no insignificant whitespace, numbers emitted as the
shortest round-tripping decimal, and no `NaN` or infinity. `files[]` is sorted
by `path` and `dependencies[]` by `id`, both in byte order — an array whose
order the exporter happened to produce would otherwise change the identity of
an unchanged project.

Canonical form is *total*: an omitted optional key is emitted with its default.
A round-trip is therefore canonical-identical rather than byte-identical to a
non-canonical input, which is what the digest needs. Unknown optional keys
round-trip verbatim. The one deliberate exception is a declared `completeness`,
which is dropped rather than preserved — keeping it would let a stale claim
outlive the rows it was computed from.

Digest spelling is fixed: `revision_id` and `parent_revision` carry a
`sha256:` prefix because they are identity references that may one day name
another algorithm. Every `sha256` field — a file row's, a dependency's,
`license_notice_sha256` — is bare lowercase hex, because the field name has
already said the algorithm.

`revision_id` is `sha256` over the canonical JSON of the whole manifest with
exactly three fields removed:

| Removed | Why |
|---|---|
| `revision_id` | It is the digest; including it would be self-referential. |
| `exported_at` | Export time is the one value that changes when nothing else did. |
| `attestations` | Signatures are computed *over* the digest. |

Everything else is covered, including `title`, `created_at`, `provenance`,
`distribution`, `executable_data` on every file row, and any unknown optional
key a newer writer emitted.

Covering nearly everything is deliberate, and the alternative is worse than it
looks. A narrower "semantic subtree" leaves the excluded fields outside the
signature, and those fields are not inert: `distribution` is what makes a
capsule Play-only, `executable_data` is what tells the user a payload contains
code, and `provenance` carries disclosure state. Leaving any of them out would
let someone edit outer metadata on a validly signed capsule and have it still
verify — turning a Play-only release into a remixable one, or hiding executable
content from the preview. So the rule is the simple one: if it is in the
manifest, it is in the identity.

The determinism properties still hold, because none of the covered fields
depends on the machine or the moment. `created_at` and `provenance` are
authored once and do not change between two exports of the same project, and
`files[]` is reduced to a canonical ordering. The digest is therefore
independent of ZIP compression level, archive member order, filesystem
timestamps, export time, and the exporting machine's paths: two exports of an
unchanged project produce the same `revision_id`.

Two capsules that differ only by export policy — a Private Backup and a Share
for Remix of the same work — get different `revision_id`s, which is correct.
They contain different bytes and grant different operations. `project_id` says
they are the same project and `parent_revision` records the lineage.

## Canonical PCM

A capsule embeds audio in exactly one portable runtime representation: a
self-describing member holding interleaved little-endian IEEE float32, one or
two channels, an integer source sample rate in `[8000, 192000]`, finite
samples only, and an exact frame count. The member's byte layout is frozen —
every audio member's identity is the SHA-256 of exactly these bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic, the ASCII bytes `pulp.pcm` |
| 8 | 4 | `decoder_version`, uint32 little-endian |
| 12 | 4 | `channels`, uint32 little-endian, 1 or 2 |
| 16 | 4 | `sample_rate`, uint32 little-endian, in `[8000, 192000]` |
| 20 | 8 | `frame_count`, uint64 little-endian |
| 28 | — | `frame_count × channels` interleaved little-endian IEEE-754 binary32 samples, exactly to the end of the member |

The header travels inside the hashed bytes, which buys three properties at
once. First, an audio row's `sha256` is simply the digest of the member bytes
— the same uniform rule every other `files[]` row follows, so the exporter's
generic member hashing and the importer's per-member extraction check need no
audio-specific side channel. Second, the member decodes from its bytes alone:
`from_canonical_bytes()` takes only the bytes, reads the geometry from the
header, and enforces `decoder_version` itself, refusing a version it does not
implement — the guard binds to the bytes rather than to metadata a consumer
could drop or forge. Third, two renditions that differ only in declared rate
have different identities, because the rate is inside the hash.
`canonical_pcm_digest()` is defined as the SHA-256 of `to_canonical_bytes()`,
bare lowercase hex. There is no separate media struct on the parse side: a
successful parse returns the header's fields in the `CanonicalPcm` itself, and
the decoder version is not surfaced because success proves it is the current
one.

Importers decode source audio through one versioned decoder
(`decode_to_canonical()`), dispatching on the container's own magic, never on
a filename. Admitted are exactly the formats that decode deterministically —
identical bytes in, identical samples out, on every ISA:

- **WAV** — RIFF/WAVE PCM16/PCM24/PCM32 and float32, including
  `WAVE_FORMAT_EXTENSIBLE` wrapping those subformats;
- **AIFF and AIFF-C** — big-endian PCM at 9–32 declared bits (sample points
  are left-justified in whole bytes, so they decode exactly at the container
  depth), plus AIFF-C `NONE`/`twos`, 16-bit `sowt`, and `fl32` float32;
- **FLAC** — via dr_flac's bit-exact integer frame decode; the
  integer-to-float conversion is this module's own power-of-two scaling, part
  of `decoder_version`, so FLAC and WAV/AIFF sources of equal content produce
  identical samples.

MP3, OGG, and AAC are **not** admitted: their float decode kernels differ
across ISAs or lose resolution, so the same file could mint different
identities on different machines. They fail with `decode_unsupported` naming
the format found, as do CAF, Ogg-wrapped FLAC, and any codec inside a
supported container that is not listed above — a refusal the user can act on
rather than an approximation they did not ask for. The user's original
compressed or high-resolution file may travel as an optional source role; it is
never required for playback on the receiving machine.

## External dependencies

An external dependency row pins a hash identity, media type, byte count,
license and attribution facts, and either an `https://` provider or a
`capsule-library:` locator. Any other provider — `http://`, `file://`, an
absolute path, a bare hostname — is refused at parse time with
`dependency_provider_denied`, because each either leaks the exporting machine's
filesystem into a shared capsule or invites a retrieval nobody can
authenticate. Which *host* is acceptable remains a consumer policy; the shape
is structural. A dependency may also declare no provider at all, in which case
it is simply not resolvable. Preview shows every provider and license
before any retrieval. With consent, retrieval writes to private staging and
verifies size and digest before decode; no version or mirror substitution is
permitted. Offline, denied, missing, hash-mismatched, and revoked dependencies
leave the capsule unopened and offer Retry, Locate, and Cancel. Cancellation
and partial downloads cannot modify the project or the shared library.

## Import state machine

1. Read the bounded root manifest only. Execute nothing.
2. Validate versions, the closure in both directions, paths, collisions,
   budgets, declared sizes against the container's own, the manifest's own
   revision digest, required roles, capabilities, and compatibility.
3. Verify the optional trust envelope: signature, publisher policy, revocation,
   version floor.
4. Produce a preview. Preview performs zero execution and zero network access.
5. Obtain explicit consent.
6. Extract only declared members into owner-private staging, verifying each
   member's digest as it lands.
7. Run profile semantic validation without executing source.
8. Publish atomically as a **new** local identity. A matching incoming
   `project_id` never replaces an existing project.
9. Compile only after admission and consent, inside the consumer's hardened
   sandbox. Last-good state survives every failure.

A signature establishes identity, not permission to escape the sandbox.

### Where member digests are checked, and why not earlier

Step 2 does not verify every member's content digest. Doing so would mean
inflating the whole archive, which is exactly what preview exists to avoid: a
capsule is a file a stranger can send, and the point of a bounded manifest read
is that inspecting one costs a manifest, not a gigabyte.

What step 2 does check is that each row's declared `bytes` matches what the
container says that member expands to — free from the central directory, and
enough to make the preview's size figures trustworthy. Content digests are
verified in step 6, as each member lands in private staging, and a mismatch
aborts before anything is published. So no unverified byte ever reaches a live
project; it simply is not verified at the moment the preview is drawn.

A consumer that wants full verification before consent can extract to a staging
area it then discards. The substrate does not do that by default because it
would make previewing a large capsule as expensive as importing one.

### Reading a staged member

A consumer never joins a manifest path to a staging root itself.
`read_staged_member()` takes the staging area and the `files[]` row, re-admits
the path under the same grammar extraction uses, performs the join inside this
layer, opens every hop relative to the staging area's pinned root without
following links, and refuses anything but a plain file.

The join is the reason the reader exists. A path rule is only worth having
where the path meets a real directory, and `staging_root / entry.path` written
in a consumer is that decision made outside the module that owns it — losing
NFC, depth, byte-budget, reserved-name, and containment checking in a place no
substrate test can see.

The size on disk must equal the row's declared `bytes`, checked before the
bytes are copied out, so the read is bounded by what the private tree holds
rather than by a number the row asserted. Content digests are not recomputed:
step 6 verified each member before it was allowed to land, and staging is
readable only by its owner. A caller wanting the stronger check hashes the
returned bytes itself.

## What export validates

An exporter must not mint a capsule this same code would refuse to read, so
`export_capsule()` holds itself to the reader's rules:

- the format identifier and version must be the ones this build writes;
- every item path is admitted under the member-path grammar and may not claim
  `capsule.json`;
- every `sha256` and `bytes` is **measured** from the bytes that travel, never
  copied from what the caller declared;
- the item set is checked for exact, case, and confusable collisions;
- the finished manifest is read back through `parse_manifest()` before the
  archive is created.

That last step is what applies the structural rules to a manifest that was
assembled in memory and never parsed. Canonical serialization applies none of
them by design — it serializes what it is handed — so the files-must-be-included
rule and the provider-shape rule would otherwise be parse-time only, and a
caller passing its own `dependencies[]` rows straight through could publish a
`file:///Users/someone/…` provider that leaks the exporting machine into a
shared capsule. Reading its own output back is deliberately how the writer
enforces them: a second copy of the rule set would be free to drift from the
one the reader uses. A rejected export keeps `parse_manifest()`'s status and
JSON-pointer subject and writes no file.

Everything a dependency row asserts about bytes that are not in the archive is
carried through as given. This layer resolves nothing and contacts no provider,
so it cannot confirm that a dependency's digest names what a recipient will
fetch.

## Error taxonomy

Every admission failure resolves to exactly one of these; a generic failure is
a defect.

`unsupported_format`, `unsupported_format_version`, `unsupported_profile`,
`unsupported_profile_version`, `unsupported_product`,
`unsupported_capability`, `missing_required_role`, `runtime_floor_too_old`,
`schema_migration_refused`, `closure_violation`, `digest_mismatch`, `unsafe_archive`,
`archive_budget_exceeded`, `path_rejected`, `path_collision`,
`manifest_invalid`, `manifest_not_first`, `missing_dependency`,
`dependency_digest_mismatch`, `dependency_provider_denied`,
`missing_licensed_sample`, `rights_insufficient`, `signature_invalid`,
`revoked_signer`, `downgrade_refused`, `creator_identity_required`,
`decode_unsupported`, `staging_failed`, `publication_conflict`, `cancelled`.

## Attestations

`attestations` is an array of signature envelopes. It is the one place a
signature may live, and it is excluded from the revision digest because each
envelope is computed *over* that digest.

```json
{
  "signer_id": "…",
  "algorithm": "ed25519",
  "signature": "base64…",
  "signed_payload_digest": "sha256:…"
}
```

`signed_payload_digest` must equal the digest the reader *computed*, never the
`revision_id` the capsule asserts about itself. Verifying against a
self-asserted value would accept any capsule that lies consistently.

A capsule with no verifier configured is admitted as **unsigned**, not as
verified. Absence of a check is never evidence that the check passed.

## Preview fields a consumer completes

`CapsulePreview::dependencies[].resolvable_locally` is always `false` as this
layer produces it. Answering it honestly needs a content-addressed library or a
provider the substrate deliberately does not have — it opens no network
connection and knows no resolver. A consumer fills the field in before showing
the preview to a person.

## Deliberate status choices

- A canonical rendition whose float32 byte count would exceed
  `max_member_expanded_bytes` is refused with `archive_budget_exceeded`, not
  `decode_unsupported`. The format is supported; the size is not, and a 2 GiB
  PCM16 source expanding to 4 GiB of floats is a real case rather than a
  hypothetical. The check happens before allocation.
- A `format_version` or `profile_version` of `0` is `manifest_invalid`, not
  `unsupported_*_version`. Zero is not a version this format ever had, so
  telling the user to upgrade would be wrong advice.
- A WAV `data` chunk whose size is not a whole multiple of the frame size is
  refused rather than truncated to whole frames. Dropping a stray trailing byte
  would change the bytes the manifest hashes.
- `canonical_pcm_digest()` can only mint a digest under the current decoder
  version, so `from_canonical_bytes()` refuses a `decoder_version` it does not
  implement rather than silently re-digesting foreign bytes as version 1.

## Member paths are generated, not user text

Member paths are produced by the exporter and restricted to a subset that is
provably already NFC without linking a Unicode character database. That subset
is narrow — it excludes Cyrillic, Greek, Thai, Devanagari, emoji, and the
supplementary planes.

That is affordable only because a member path is never the user's filename. A
sample is content-addressed, and the name the author chose travels as metadata
inside the bank manifest, where arbitrary text is fine. An exporter that put a
user-supplied filename into a member path would break for a large fraction of
the world's users, and the admission rule is what makes that failure loud
rather than silent.

`admit_member_path()` verifies and returns the path unchanged. It deliberately
does not rewrite bytes toward NFC: with no character database available, the
only sound options are *prove already-NFC* or *reject*, and quietly returning a
half-normalized path would be worse than either.

## Profile registry

A consumer registers a profile codec and validator by profile identifier and
version. The registry has no compile-time dependency on any consumer. An
unregistered required profile yields `unsupported_profile` with the exact
identifier and version the capsule asked for, so the product can name the
download the user needs.

The substrate enforces `required_roles()`; a profile publishes the vocabulary
and this layer checks it without understanding what any role means. Preview
reports each role absent from `files[]` and `dependencies[]` as
`missing_required_role`, carried as an `unsupported` verdict rather than an
immediate error — preview exists to say what is missing — and admission refuses
on that verdict before extracting anything, so nothing reaches the profile's
`validate_staged()` with a required role missing. A consumer that re-checks is
duplicating a rule it does not own. Only the missing direction is checked: a
role the profile does not list is not rejected, because unknown optional
metadata must round-trip and deciding which unrecognized roles matter is the
profile's own job.
