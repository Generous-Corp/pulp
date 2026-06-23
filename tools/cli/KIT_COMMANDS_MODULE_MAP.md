# Kit Commands Module Map

This map records the ownership contract for `tools/cli/kit_commands.cpp` and
its extraction targets. The file currently owns the whole local kit/package
workflow in one 3,927-line translation unit: manifest validation,
publish-policy checks, registry-manifest signature checks, archive
hashing/extraction, apply/remove filesystem mutation, validation-profile
verification, pack/publish/init helpers, and command dispatch.

Extraction changes should preserve the contracts below or update this file, the
focused tests, and `tools/scripts/hotspot_size_guard.json` in the same change.

## Target Modules

| Module | Owns | Must not own |
| --- | --- | --- |
| `kit_manifest_validation.{hpp,cpp}` | `pulp.package.json` parsing, required-field checks, schema/kind/capability validation, dependency/package metadata shape, declared-path and evidence-hash validation, and the public `validate_manifest_path` result contract. | CLI argv parsing, archive extraction, project filesystem mutation, dependency registry resolution, screenshot execution, or publish readiness. |
| `kit_policy.{hpp,cpp}` | Local policy checks over an already parsed manifest: content-pack lane rejection, publish dry-run policy, agent-authoring review requirements, platform/realtime compatibility policy, and dependency package readiness checks. | JSON parsing, archive extraction, filesystem writes, zip/hash I/O, CLI dispatch, or registry-manifest signature verification. |
| `kit_registry_manifest.{hpp,cpp}` | Canonical registry-manifest signed-message construction and Ed25519/public-key/signature/digest checks for publish dry-run. | Package archive extraction, project apply/remove mutation, CLI output formatting, or general manifest validation. |
| `kit_archive.{hpp,cpp}` | `.pulpkit` / `.pulpcontent` archive detection, safe relative-entry validation, `files.sha256.json` verification, temporary extraction, pack file collection, SHA manifest generation, and zip writing. | Project apply/remove semantics, dependency registry lookup, publish policy, validation-profile execution, or CLI dispatch. |
| `kit_apply.{hpp,cpp}` | Reviewed project mutation: lockfile read/write, generated CMake include management, declared path copy, ownership markers, rollback snapshots, remove/uninstall, and backup cleanup. | Manifest schema validation, archive extraction internals, publish dry-run policy, registry-manifest signature checks, or screenshot execution. |
| `kit_profiles.{hpp,cpp}` | Validation profile verification after plan review: screenshot profile shape, signal-graph/state fixtures, node-pack manifest metadata, native component file claims, and optional screenshot execution. | Manifest required-field validation, apply/remove mutation, archive packing/extraction, registry publish policy, or CLI argv parsing. |
| `kit_command_dispatch.{hpp,cpp}` | `pulp kit` argv parsing, command help, human/JSON output routing, and thin calls into the focused modules above. | Business rules that can be unit-tested without CLI argv, filesystem mutation details, cryptographic checks, archive traversal rules, or validation-profile execution internals. |

## Boundary Rules

- Manifest validation stays side-effect-free except reading `pulp.package.json`
  and explicitly declared local files needed to verify paths and evidence
  hashes.
- Inspect, plan, and publish dry-run must not execute CMake, JavaScript,
  scripts, dynamic libraries, or node-pack binaries.
- Archive helpers must reject absolute paths, `..` traversal, unsafe extraction
  destinations, payload files missing from `files.sha256.json`, and
  payload/hash-list mismatches before a manifest from the archive is trusted.
  Local tree pack/copy paths must keep rejecting symlinks; if archive metadata
  can represent links, extraction must reject those before trusting payloads.
- Apply/remove is the only layer that mutates a project. It must own rollback,
  lockfile ownership records, generated CMake updates, and deletion safety.
- Command dispatch remains a thin adapter. New kit behavior should land behind
  a focused module API first, then be wired into `cmd_*` parsing.
- Content-pack manifests may be searched, validated, and inspected through the
  kit surface, but `plan`, `apply`, and `publish` must keep rejecting them and
  direct callers to `pulp content`.

## Current Test Anchors

- `test/test_cli_kit_commands.cpp` is the primary behavioral corpus for kit
  validation, inspect/plan/apply/remove/pack/publish/init behavior, JSON output,
  archive safety, and rollback/error cases.
- `test/test_cli_content_commands.cpp` exercises the neighboring content-pack
  lane and catches kit/content boundary drift.
- `test/cmake/content_workflow_tests.cmake` builds
  `pulp-test-cli-kit-commands`; keep new implementation files in that target
  whenever code moves out of `kit_commands.cpp`.
- `tools/scripts/hotspot_size_guard.py` freezes `kit_commands.cpp` at the
  current 3,927-line baseline. Every extraction PR that shrinks it must lower
  the ceiling to the new exact LOC.

## Extraction Order

1. Move manifest parsing and shape validation into
   `kit_manifest_validation.{hpp,cpp}` while keeping the existing
   validation-result JSON/error corpus unchanged.
2. Move platform/realtime/content-lane/publish-readiness policy helpers into
   `kit_policy.{hpp,cpp}` so manifest parsing does not grow new policy rules.
3. Move registry-manifest signing checks into `kit_registry_manifest.{hpp,cpp}`
   with focused signature/digest mismatch tests.
4. Move archive pack/extract helpers into `kit_archive.{hpp,cpp}` and keep the
   traversal/hash/symlink fixtures in the kit command test corpus.
5. Move plan/apply/remove mutation helpers into `kit_apply.{hpp,cpp}` with
   rollback and ownership-record tests.
6. Move validation-profile verification into `kit_profiles.{hpp,cpp}` so
   optional screenshot execution cannot leak into inspect/plan.
7. Leave `kit_command_dispatch` as the final adapter once the behavior modules
   are stable and covered.
