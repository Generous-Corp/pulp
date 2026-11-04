# G-3 identity contract

Status: implementation, evidence-gated after G1 port-name delivery.

## Decision

Preserve the authored name as the identity of a port. Canonical numbering is a
derived view for transport, display, and lookup tables; it must never rewrite
the authored identity when ports are reordered, aliased, inserted, or removed.
An authored name is compared after the encoding normalization below. A
canonical number may change when order changes, but that change is not an
identity change.

G1 port names are now available through the existing `PortHandle` owner
surface. The control-side implementation may therefore expose the value-owned
identity helpers described below; runtime scheduling and graph mutation remain
outside this contract.

## Required behavior

| Case | Required result | Control |
| --- | --- | --- |
| Same authored names, same order | Positive; identities match | Compare normalized authored names pairwise |
| Same authored names, reordered | Positive; identities still match | Match by normalized name, then verify the new order separately |
| One authored alias renamed to its declared alias | Positive only when the alias is declared by the source contract | Resolve alias to the authored identity; do not treat an arbitrary display label as an alias |
| Two ports with the same name | Negative; identity is ambiguous | Reject the set and report the colliding normalized name |
| Name changed without a declared alias | Negative; this is a new identity | Do not canonicalize or silently migrate |
| Name removed or added | Negative for the affected identity set | Report the missing and added names |
| Canonical numbers differ after reorder | Positive if authored-name matching succeeds | Never use the number as the identity key |
| Empty name | Negative | Empty or whitespace-only names are invalid authored identities |
| `NaN`, `+Inf`, or `-Inf` in an identity-bearing numeric field | Negative | Reject before comparison or serialization; these values are not valid contract data |
| Non-finite values in an unrelated diagnostic | Negative for the record containing them | Fail closed rather than emitting non-standard JSON |

## Encoding and normalization

Identity values are Unicode strings encoded as UTF-8. Inputs MUST be valid
Unicode scalar values and MUST NOT contain an embedded NUL. Normalize to NFC
before comparison and serialization. Comparison is exact after NFC: it is
case-sensitive, preserves punctuation, and does not apply locale folding,
ASCII transliteration, trimming, or whitespace collapsing. A name containing
leading or trailing whitespace is therefore distinct from the same name
without it; producers should reject such names rather than silently rewrite
them. Serialization uses UTF-8 JSON with non-ASCII characters preserved (no
ASCII escape requirement), and rejects invalid UTF-8, unpaired surrogates,
control characters, and non-finite JSON numbers.

Aliases use the same normalization and encoding rules. An alias record must
name both the authored identity and the replacement spelling, must be
unambiguous within the port set, and must be versioned with the source
contract. Alias resolution is one hop; chains and cycles are invalid.

## Positive and planted-negative controls

Every implementation or fixture review must include the following controls:

1. Positive: `cutoff`, `drive`, and `mix` match after a reorder while their
   canonical numbers change.
2. Positive: a declared alias maps `drive` to `Drive` and retains the authored
   identity `drive`.
3. Negative: `drive` becomes `gain` without an alias.
4. Negative: two inputs normalize to the same authored name.
5. Negative: an empty or whitespace-only authored name.
6. Negative: a JSON fixture containing `NaN`, `Infinity`, or `-Infinity`.
7. Negative: a fixture with invalid UTF-8, an embedded NUL, an unpaired
   surrogate, or a non-NFC spelling that is not normalized before comparison.

The negative controls are planted: a checker must prove that it rejects each
fixture, rather than merely showing that the positive fixtures pass. A report
must include the normalized name, source location when available, and the
reason for rejection without exposing a lossy replacement string as identity.

## Migration and dependency boundary

The G1 port-name delivery is present in the owner surface. The implementation
exposes authored names and aliases through the value-owned control-side API and
the controls above are executable in its conformance lane. No runtime mutation,
serializer, or compatibility behavior is implied by this contract. Canonical
renumbering remains an internal derived index.
