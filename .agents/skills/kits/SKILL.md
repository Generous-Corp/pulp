---
name: kits
description: Search, inspect, plan, apply, remove, pack, and scaffold local Pulp package manifests. Use for Pulp-native source/UI/template kits and for untrusted or semi-trusted manifest-bearing artifacts that must be reviewed before project mutation.
requires:
  scripts:
    - tools/kits/pulp-package.schema.json
    - tools/kits/pulp-registry-manifest.schema.json
---

# Pulp Kits

Use this skill when a user has a local Pulp kit directory, `.pulpkit` archive, wants to create one, or asks whether an external manifest-bearing artifact is safe to compose into a project.

Kits are valuable because:

- developers share real Pulp source, UI, templates, validation fixtures, graph nodes, and native components instead of copy-pasted examples;
- users see capabilities, licenses, files, realtime/platform claims, and project changes before approval;
- agents inspect and plan from structured metadata without executing untrusted package code.

## Trust Boundary

`pulp add <name>` is for curated dependency packages from Pulp-controlled registry metadata.

`pulp kit ...` is for local or external Pulp-native artifacts that may transform a project, install files, or introduce executable source. The workflow is inspect, plan, verify, approve, apply.

Trust rules:

- metadata commands never run package CMake, JavaScript, scripts, dynamic libraries, remote search, or content installers;
- `.pulpkit` archives must include `files.sha256.json`, and every payload file must be listed and hash-matched before the manifest is trusted;
- apply requires explicit approval and writes only owned project files;
- remove uses `.pulp/kits.lock.json` ownership records and is constrained to `pulp-kits/<kit-id>/...` plus known generated lock/CMake files; because a kit removal spans a set of files, the OK line names the count it deleted — `Removed kit <id> (removed N files)` — rather than a single path, for parity with `tool uninstall`;
- packing and publish dry-run never execute package code.

## Commands

```bash
./build/pulp kit validate <path>
./build/pulp kit search <query> --root <dir> --lane kit --json
./build/pulp kit search <query> --root <dir> --lane content --json
./build/pulp kit validate <path> --json
./build/pulp kit inspect <path> --json
./build/pulp kit plan <path> --project <dir> --json
./build/pulp kit verify <path> --project <dir> --json
./build/pulp kit verify <path> --project <dir> --execute-screenshots --json
./build/pulp kit apply <path> --project <dir> --yes
./build/pulp kit remove <kit-id> --project <dir> --yes
./build/pulp kit pack <path> --output <file> --json
./build/pulp kit publish <path> --dry-run --json
./build/pulp kit publish <path> --dry-run --registry-manifest <file> --json
./build/pulp kit init --kind source --id com.example.my-kit --dir ./my-kit
./build/pulp create "Kit Gain" --template ./my-template-kit --no-build --ci
```

Validation reads `pulp.package.json` plus declared local files only. It checks shape, licenses, Pulp/C++ requirements, known Pulp module dependencies, and declared evidence hashes before plan/apply. `content-pack` manifests can be searched, validated, and inspected, but `pulp kit plan/apply/publish` rejects them; switch to `pulp content ...`.

Developer notes:

- `pulp kit search` is local discovery only; it never fetches and never makes a result trusted.
- `pulp kit publish --dry-run` is local registry-readiness only. Remote registry submission is disabled.
- Agent-authored packages need `authoring.humanReview.reviewed: true` before publish dry-run can pass.
- Template kits need `validation.generatedProjectDiffs` before `pulp create --template <kit-dir>`.
- UI kits need screenshot evidence. Run `pulp kit verify` after plan review; use `--execute-screenshots` only when rendered artifacts are explicitly needed.
- `--execute-screenshots` runs the screenshot tool via `pulp::platform::exec` (argv vector, no shell) in `maybe_execute_screenshot_profile`. Keep it off `std::system`: on Windows a `cmd /c` command that starts with a quoted tool path plus further quotes mis-parses and never launches the tool — see the `exec`-vs-`std::system` gotcha in the `cli-maintenance` skill.
- After applying a UI kit, recommend `pulp_use_kit_ui(...)` only when the developer chooses to attach the reviewed script/tokens/assets.
- Graph/native kits must surface exported fixtures/files, platform claims, and realtime claims before apply. Verification must not load dynamic libraries.
- Signed `node-pack` kits must not claim iOS/AUv3 support.

## Agent Workflow

1. Search only local roots with `pulp kit search` when discovery is needed; do not treat results like curated `pulp add` packages.
2. Validate or inspect the kit. If the result is `lane: content` or `kind: content-pack`, switch to the `pulp content` workflow before preview/install.
3. Explain capabilities, licenses, dependency package ids, paths, and validation issues.
4. Make the trust boundary explicit: curated dependencies use `pulp add`; project-transforming artifacts use `pulp kit`.
5. For a template kit creating a new project, run `pulp create "<name>" --template <kit-dir> --no-build --ci` after validation. Use a real build without `--no-build` when you need proof that exported format targets compile; generated tests may be skipped if the standalone SDK/project does not provide Catch2.
6. If existing-project mutation is requested, run `pulp kit plan <path> --project <dir> --json` and present the actions.
7. Run `pulp kit verify <path> --project <dir> --json` when declared validation profiles should be evaluated after plan review; add `--execute-screenshots` only when the reviewed plan should also produce Pulp-rendered screenshot artifacts.
8. Apply only after explicit approval, using `pulp kit apply <path> --project <dir> --yes`.
9. Remove only after explicit approval, using `pulp kit remove <kit-id> --project <dir> --yes`.
10. Pack archives with `pulp kit pack <path> --output <file> --json` when distribution is requested.
11. Run `pulp kit publish <path> --dry-run --json` before recommending registry submission; include `--registry-manifest <file>` when signed registry metadata is available.
12. For UI kits, surface declared screenshot profiles and reports as evidence before recommending apply; if `--execute-screenshots` was used, include the generated artifact paths, render logs, visual diff reports, and any declared tolerance.
13. For graph/native kits, surface exported graph/state fixtures, node-pack manifests, native component files, platform claims, and realtime claims before recommending apply.
14. Do not run package CMake, JavaScript, scripts, dynamic libraries, or network fetches during search/validate/plan/apply/remove/pack/create-from-template/publish-dry-run.

Use MCP tools when available:

- `pulp_kit_validate`
- `pulp_kit_search`
- `pulp_kit_inspect`
- `pulp_kit_plan`
- `pulp_kit_verify`
- `pulp_kit_apply`
- `pulp_kit_remove`
- `pulp_kit_pack`
- `pulp_kit_publish_check`
- `pulp_kit_init`

## Related extend surfaces

`packages`, `kits`, `content`, and `installable-tools` are Pulp's four ways to
extend a project or machine, and they share one lifecycle contract: **add is
validated, remove is confirmed + confined to the surface's own area + names what
it deleted, and both add and remove ship tests.** Pick the right surface and read
the shared contract in
[extending-pulp.md](../../../docs/reference/extending-pulp.md).

- [`packages`](../packages/SKILL.md) — third-party audio DSP libraries → a project
- [`kits`](../kits/SKILL.md) — reusable Pulp code/UI/templates → a project
- [`content`](../content/SKILL.md) — data-only packs (presets/samples) → an installed plugin
- [`installable-tools`](../installable-tools/SKILL.md) — machine-level dev/agent tooling under `~/.pulp/tools/`, plus the shared validate-and-uninstall-from-outside-a-checkout bar
