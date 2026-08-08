# Shipping a capability-controlled endpoint

Release artifacts are inspector-free by default. `PULP_ENABLE_INSPECTOR=ON`
only makes optional SDK components available; it does not link a listener,
discovery publisher, server, registration, or runtime evaluator into an
ordinary `pulp_add_plugin` target.

The consolidated authoring and diagnostics reference is
[Capability control](../reference/capability-control.md). Every
`pulp_add_plugin` target emits one canonical
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
Mutation control capabilities require `dev.pulp.session/control@1`. The
installed per-user broker composes trusted T0/T1 enrollment and routing;
ordinary plugin-format targets stay stripped and unsupported host tiers remain
unavailable.

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

Every plugin and standalone binary runs a post-link control shipping scan.
This includes Standalone, VST3, CLAP, LV2, AU v2, both executable AUv3 pieces,
the AUv3 container, and AAX when that SDK is available. Multi-plugin VST3 and
CLAP bundles enter the same scanner as ordinary `production-stripped`
artifacts; the bundle helper is not a packaging bypass.
Control endpoints are Standalone-only: in a mixed developer build, each
non-Standalone artifact receives its own `production-stripped` manifest rather
than inheriting the intentional Standalone profile.

The scanner verifies the canonical manifest digest together with retained
profile, format, platform, and architecture markers. It measures the actual
artifact size and records which native symbol and dependency scanners ran in a
`<target>.<format>.control-shipping-report.json` sidecar. On macOS those tools
are `nm`, `otool`, and `lipo`; Linux uses `nm` and `readelf`; Windows uses
`dumpbin`. A missing native scanner blocks the artifact, so each platform lane
must provide its native toolchain rather than silently claiming evidence.

For an intentional profile, every declared capability and endpoint marker must
be retained from the linked control implementations; the shipping helper emits
only artifact identity and cannot make an empty target satisfy its declaration.
Source-bearing control components own these markers, and a real-component link
fixture constructs the endpoint and resolves a declared capability before its
final executable is scanned.
The high-risk evaluator marker must exactly match its separate acknowledgement.
For `production-stripped`, the scanner rejects endpoint,
capability, runtime-evaluation, and Remote View authority strings, known control
symbols, and known control dynamic dependencies. It also scans native binaries
inside the package closure (or resolved sibling loader dependencies), so
renaming a helper library cannot hide retained control code. The check reads
the final linked artifact, so a CMake option or an unlinked declaration is not
accepted as shipping proof.

The generated `dev.pulp.control/shipping-artifact@1` sidecar is per binary and
names its format, platform, complete architecture list, profile, and canonical
manifest digest. This makes diagnostic and research artifacts visibly distinct
from ordinary production output while preserving the canonical standalone
manifest used by the read-only audit command.

Custom `pulp-install-<target>` targets depend on every format binary they copy.
That dependency is load-bearing: installation cannot copy a stale format while
skipping its post-link scan. A persisted scan stamp depends on the artifact,
both manifests, and the scanner itself, so changing shipping policy invalidates
an earlier report even when the binary did not relink. The helper and scanner
are both exported in the installed CMake SDK, and an installed-layout test
builds the complete profile and format policy matrix without reaching back into
the Pulp source tree.

The repository matrix exercises all five profiles and the Standalone, VST3,
CLAP, LV2, AU v2, AUv3, and AAX policy labels. The local macOS proof builds
universal `arm64` and `x86_64` artifacts and verifies both slices with `lipo`.
The path-scoped `Control shipping native matrix` workflow closes the native CI
boundary with real installed-SDK consumers: macOS universal, Linux `x86_64`
and `aarch64`, and Windows `x64`. It builds every available real plug-in format
for each platform and aggregates the canonical `nm`/`otool`/`lipo`,
`nm`/`readelf`, or `dumpbin /UNDNAME` reports into
`dev.pulp.control/native-shipping-evidence@1`. A synthetic format label does
not count as proof.

AAX remains developer-supplied. On a protected-main push, when both
`PULP_AAX_SDK_ZIP_URL` and `PULP_AAX_SDK_ZIP_SHA256` repository secrets exist,
the macOS universal leg builds and scans a real AAX bundle from the verified
out-of-tree SDK. Pull requests and manual runs never receive those secrets; they
record `status: unavailable`, `proof: false`, and
`aax-sdk-secret-withheld-untrusted-event`. A trusted run without the URL uses
`aax-sdk-secret-unavailable`. Both are explicit availability dispositions, not
AAX proof, and a configured URL without its checksum fails closed.

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

Shipping evidence and runtime authority remain separate checks. The canonical
trusted launcher, host router, typed execution, CLI/MCP clients, bounded
artifact store, and telemetry path do not restore the deleted legacy authority;
new support must extend the same centralized path.

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
