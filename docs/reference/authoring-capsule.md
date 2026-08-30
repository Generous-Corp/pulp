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
| `policy.redistribution` | `allowed`, `restricted`, `unknown` |
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

A capsule embeds audio in exactly one portable runtime representation:
interleaved little-endian IEEE float32, one or two channels, an integer source
sample rate in `[8000, 192000]`, finite samples only, and an exact frame count.
The digest covers the canonical PCM bytes together with the rate, channel
count, and frame count.

Importers decode allowed source WAV PCM16/PCM24/PCM32/float32 through one
versioned decoder. Any other codec or channel layout fails with
`decode_unsupported` rather than being approximated. The user's original
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

## Error taxonomy

Every admission failure resolves to exactly one of these; a generic failure is
a defect.

`unsupported_format`, `unsupported_format_version`, `unsupported_profile`,
`unsupported_profile_version`, `unsupported_product`,
`unsupported_capability`, `runtime_floor_too_old`, `schema_migration_refused`,
`closure_violation`, `digest_mismatch`, `unsafe_archive`,
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
