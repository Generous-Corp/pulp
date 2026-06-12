# /kit

Search, inspect, preview, approve, and apply local Pulp kit manifests or `.pulpkit` archives.

Use this for Pulp-native source, UI, template, validation, graph, and native-component artifacts that should be reviewed before project mutation.

Why it matters:

- developers can share real Pulp building blocks instead of copy-pasted examples;
- users see capabilities, licenses, files, and project changes before approval;
- agents can inspect and plan without running untrusted package code.

Keep curated dependency packages on `pulp add <name>`. Use `pulp kit` when the artifact may transform project structure, install files, or introduce executable source. `.pulpkit` archives must include `files.sha256.json`; every payload file must be listed there and hashes must match before validation, planning, verification, apply, or publish dry-run.

Common commands:

```bash
pulp kit validate <path>
pulp kit search <query> --root <dir> --lane kit --json
pulp kit search <query> --root <dir> --lane content --json
pulp kit validate <path> --json
pulp kit inspect <path> --json
pulp kit plan <path> --project <dir> --json
pulp kit verify <path> --project <dir> --json
pulp kit verify <path> --project <dir> --execute-screenshots --json
pulp kit apply <path> --project <dir> --yes
pulp kit remove <kit-id> --project <dir> --yes
pulp kit pack <path> --output <file> --json
pulp kit publish <path> --dry-run --json
pulp kit publish <path> --dry-run --registry-manifest <file> --json
pulp kit init --kind source --id com.example.my-kit --dir ./my-kit
```

Workflow:

1. Validate or inspect the manifest.
2. Plan to preview project changes.
3. Verify declared validation profiles after reviewing the plan.
4. Apply only after explicit approval.
5. Remove only from constrained lock-recorded kit ownership.

Trust rules:

- metadata commands never run package CMake, JavaScript, scripts, dynamic libraries, remote search, or content installers;
- archives must include `files.sha256.json`, and every payload file must be listed and hash-matched before the manifest is trusted;
- validation checks manifest shape, licenses, Pulp/C++ requirements, known Pulp module dependencies, and declared evidence hashes before plan/apply;
- `content-pack` manifests can be searched, validated, and inspected, but `pulp kit plan/apply/publish` rejects them; use `pulp content ...`;
- apply writes only owned files and records the reviewed manifest digest in `.pulp/kits.lock.json`;
- remove deletes only constrained lock-recorded kit paths under `pulp-kits/<kit-id>/...` plus known generated lock/CMake files.

Prefer structured authoring provenance. `authoring.createdBy.type` should be `human`, `agent`, or `mixed`; agent-created packages are not publish-ready until `authoring.humanReview.reviewed` is true.

Developer notes:

- `pulp kit search` is local discovery only; it never fetches and never makes a result trusted.
- Template kits need `validation.generatedProjectDiffs` before `pulp create --template <kit-dir>`.
- UI kits need screenshot evidence. Run `pulp kit verify` after plan review; use `--execute-screenshots` only when rendered artifacts are explicitly needed.
- After apply, use `pulp_use_kit_ui(...)` only when the developer chooses to attach the reviewed script/tokens/assets.
- Graph/native kits must surface exported fixtures/files, platform claims, and realtime claims before apply. Verification must not load dynamic libraries.
- Signed `node-pack` kits must not claim iOS/AUv3 support.
