# Shipping a capability-controlled endpoint

Release artifacts are inspector-free by default. `PULP_ENABLE_INSPECTOR=ON`
only makes optional SDK components available; it does not link a listener,
discovery publisher, server, registration, or runtime evaluator into an
ordinary `pulp_add_plugin` target.

Every `pulp_add_plugin` target now emits one canonical
`dev.pulp.control/artifact-manifest@1` sidecar. With no declaration, the target
uses `production-stripped`: no endpoint and no capabilities. An intentionally
inspectable developer edition declares one profile and its exact stable
capability IDs:

```cmake
pulp_add_plugin(MyDeveloperEdition
    FORMATS Standalone
    CONTROL_PROFILE developer-local
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.state/read@1
        dev.pulp.ui/observe@1
        dev.pulp.diagnostics/read@1
        dev.pulp.logs/read@1
        dev.pulp.ui/capture@1
        dev.pulp.telemetry/subscribe@1)
```

This declaration emits a retained capability marker plus
`<target>.inspector-capabilities.json`. It does not link or activate a live
standalone endpoint. The legacy server, raw client, discovery publisher, and
standalone session owner were deleted in Phase 3; the manifest remains an upper
bound for the canonical replacement, not evidence of current reachability.

The Phase 4 runtime archives now implement exact T0/T1 instance status and
bounded state/parameter catalog reads. They are reachable only through a
carrier-authenticated `ControlService` session, an exact registration grant,
and an injected runtime executor. There is no `pulp inspect` or MCP adapter for
these operations yet, and ordinary production artifacts remain stripped.

Control declarations are re-read as configure-time truth on every CMake run.
Changing a target from `research-unsafe` to a narrower profile, removing
`runtime.eval`, or deleting its acknowledgement force-replaces the cached
profile and capability values in the existing build directory. Developers do
not need to delete the build tree to withdraw authority, and stale cache state
cannot preserve a removed capability.

Available profiles are `production-stripped`, `developer-local`,
`test-deterministic`, `support-diagnostics`, and `research-unsafe`.
`support-diagnostics` accepts only instance, state, diagnostics, and log reads.
Control capabilities require `dev.pulp.session/control@1`. No product endpoint
is currently composed; ordinary plugin-format targets stay stripped.

`dev.pulp.runtime/evaluate@1` is arbitrary execution in the product process.
No profile or acknowledgement implies it. A target that truly needs it must use
the `research-unsafe` profile, declare the capability, and add the distinct
acknowledgement keyword:

```cmake
    CONTROL_PROFILE research-unsafe
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.session/control@1
        dev.pulp.runtime/evaluate@1
    ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL
```

The legacy `SHIP_INSPECTOR`, `SHIP_INSPECTOR_RUNTIME_EVAL`, and
`INSPECTOR_CAPABILITIES` spellings have been removed. Use the canonical control
profile and capability declarations.
They cannot be mixed with `CONTROL_*`; new projects should use only the
canonical form.

`BUNDLE_ID` remains optional for a stripped legacy-compatible target. It is
required as soon as a control profile or endpoint is declared, so an enabled
endpoint can never ship with anonymous publisher identity.

Packaging repeats the review boundary. Use `--ship-inspector` for an endpoint
declared by the build manifest. If and only if that manifest includes
`runtime.eval`, also pass `--ship-inspector-runtime-eval`. Manifest/flag
mismatches fail before packaging. `pulp validate --json`, `pulp ship check
--json`, and `pulp ship package --json` include the capability report; package
also writes `artifacts/inspector-capability-package-input.json`.
JSON package failures remain JSON and include a nonzero `exit_code` plus the
fail-closed diagnostic.

Every standalone build runs a manifest-versus-binary scanner using retained,
Pulp-specific shipping and capability markers. Intentional artifacts fail if
their endpoint or high-risk evaluator marker is missing, or if the evaluator
appears without its separate acknowledgement. Generic class or symbol names
are not treated as proof because unrelated product code may use the same text.
The scanner also rejects legacy Remote View parameter authority from
`production-stripped` artifacts. OSC UDP is reported as a separate external
surface, not misrepresented as Product A protection.

Directory and direct-file audits resolve artifact names only as safe basenames
beside their sidecars. An exact-named direct sidecar is not sufficient by
itself: its target or product identity must also match the artifact filename
(including the supported `.exe` form). Absolute names, `..` traversal,
identity mismatches, and symlinked artifacts or sidecars are rejected rather
than followed or consent-bound to a different publication. Canonical evidence
in both direct-file and directory audits additionally requires the sidecar stem to equal
the manifest target and resolves only executables named by the manifest target
or product; marker-bearing renamed siblings cannot inherit stale identity.
Plugin-format subtrees (`.vst3`, `.clap`, `.component`, `.appex`, `.aaxplugin`,
and `.lv2`, case-insensitively) are excluded from standalone evidence, including
their sidecars. Direct audits classify both the absolute caller path and its
resolved path, so a working-directory-relative spelling or parent symlink alias
cannot hide a plugin-format ancestor.
Each candidate executable is read into one immutable audit snapshot and those
same bytes drive
standalone selection, surface detection, marker verification, artifact hashing,
and consent identity. The audit never selects one version of a file and hashes
or approves a later version.

The manifest contains an opaque build-tree ID and the frozen
control-registry digest in addition to its exact capabilities. Both participate
in the embedded manifest SHA-256. The audit derives the durable consent key as
SHA-256 of the verified manifest digest plus the SHA-256 of the exact artifact
bytes. Any binary, identity, capability, risk, or schema change therefore
produces a different `consentIdentity`. The audit rejects a stale registry digest and a
directory that contains no auditable Pulp standalone artifact; arbitrary
plugin bundle directories cannot receive an empty `pass`.

Before shipping, run the same read-only check yourself:

```bash
pulp inspect audit path/to/MyProduct --json
```

The audit does not load or activate the artifact. It verifies the canonical
manifest, build and registry identity, profile and SHA-256 marker, declared capability markers, endpoint and
runtime-eval boundaries, and known external surfaces. Each capability includes
its risk, side-effect class, executor, and expected evidence, with focused
advice for mutation or critical authority. Exit 0 is `pass`; exit 1 is a
fail-closed `block`; command misuse exits 2.
Legacy shipping manifests remain readable by compatibility report paths, but
the control audit blocks them with `audit.canonical-manifest-required`; they
cannot produce identity-bound consent. Every block includes a stable
`manifest.*` or `audit.*` `errorCode` in JSON.
The report includes both `artifactDigest` and the derived `consentIdentity`, so
code changes cannot retain consent merely by reusing a build-tree manifest.
The command is deliberately included in Inspector-disabled production SDKs.

This intentional temporary capability reduction keeps shipping evidence useful
while Phases 4–7 finish the canonical trusted launcher and host adapters,
broker-routed typed execution, client migration, and final release
negative-controls. None of that work may restore the deleted legacy authority.

The frozen operation schemas are closed and bounded. Required input and output
resource, receipt, lease, stream, plugin, build, node, and idempotency
identifiers cannot be empty; strings and collection sizes have
explicit ceilings; capture uses distinct window and node request shapes; state
parameter IDs cover the full unsigned 32-bit domain; transport tempo is
20–400 BPM; and telemetry accepts at most 32 unique, nonempty channel IDs.
Trace sessions freeze the concrete 1–512 MiB ring and bounded category
contract, and the host-main executor enforces the same unique, nonempty,
128-category/128-byte limits before capture. Other Performance, Audio, and
Motion controls use closed action variants; motion-start accepts 1–32 declared
metrics. Before creating a trace, the executor rejects unknown fields, empty or
overlong UTF-8 names and node IDs, duplicate or unknown property names, and
invalid geometry spaces or sources. Name ceilings count Unicode codepoints,
not encoded bytes. Its receipt must echo
its action and preserve the returned trace ID so a later stop can name it.
Trace-session start and stop reject undeclared translated fields before changing
capture state. Schema-integer fields—including trace-session `ring_mb` and Motion frame/FPS
values—accept finite, in-range integral JSON numbers such as `15.0` and
`15e+0`, as required by Draft 2020-12. Authoring bypass and lock
require a nonempty anchor of at most 256 Unicode codepoints, while highlight
requires an exact node with a 256-byte UTF-8
ceiling. Runtime evaluation
rejects NUL and carries both a character ceiling and the executor's
65,536-byte UTF-8 ceiling.
Changing any of these limits changes the registry digest and therefore the
artifact and consent identity reviewed above.
