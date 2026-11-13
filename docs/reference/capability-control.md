# Capability control

Pulp has one local capability-control authority: the per-user broker. The CLI,
MCP server, trusted T0 jobs, and trusted T1 standalone hosts are clients or host
adapters of that authority. A manifest, tool description, or live process never
grants authority by itself.

An operation runs only when every term is true:

```text
implemented ∩ built ∩ host_available ∩ activated
∩ policy_eligible ∩ client_granted ∩ session_live
```

Use `pulp control status --instance ID --explain` to inspect these terms for one
exact broker-issued instance ID. There is no host, port, newest-instance,
human-label, discovery-file, or raw protocol selector.
Status observes registration only: without an operation to evaluate,
`implemented`, `built`, `host_available`, `activated`, and `session_live` are
reported as `not_evaluated`, never inferred as satisfied from registration.

## Keep the five exposure surfaces separate

A public sequencer API is not automatically controllable, and an MCP tool is
not automatically a Product A operation. Pulp records five independent
surfaces:

| Surface | What it proves | What it does not prove |
|---|---|---|
| Installed SDK | A consumer can compile against and link the public API. | A CLI, MCP, or live operation exists. |
| Offline timeline CLI | The repository CLI has a real typed definition and handler for a persisted timeline operation. | The operation is remotely callable or authorized against a live product instance. |
| Offline timeline MCP | The MCP server has a real typed definition and handler for a persisted timeline operation. | The per-user broker granted live authority. |
| Live Product A control | The canonical registry, operation, executor, policy, CLI projection, MCP projection, and tests compose through the broker. | An unrelated build manifest or offline tool can issue a grant. |
| Design-time agent manifest | The installed SDK describes an offline algorithm or composition capability to an authoring agent. | Runtime discovery, transport, consent, or control authority exists. |

The design-time manifest in `docs/status/agent-capabilities.json` is deliberately
separate from the Product A registry. Do not add an entry there merely to make a
live-control gap appear closed. Conversely, an offline CLI/MCP pairing remains
useful even when a live Product A adapter is not yet appropriate.

## Sequencer exposure ledger

The durable delivery ledger for sequencer and playback slices is carried in two
places that assemble into one document: `docs/status/sequencer-exposure.json`
holds the rows written before the ledger was split, and
`docs/status/sequencer-exposure/` holds one file per row —
`rows/<row-id>.json`, `tombstones/<row-id>.json`, and `ledger.json` for the
schema version, ledger ID, and audit block. A new row is written as its own
file. Each row lives in exactly one place: a row ID carried by both forms, or a
header carried by both forms, is an error rather than a silent winner.

The directory exists for a mechanical reason. A single document has one append
point, so two branches that each add a row rewrite the same bytes and conflict
with each other even when each merges cleanly against `main`, and the merge
queue cannot batch them at all. Two row files share no bytes.

A feature change records a `pending` row with
its durable claim ID and exact owned paths before it can know its protected
merge identity. A post-merge ledger follow-up promotes that same row to
`released` and pins the PR, accepted source head, and protected merge SHA.
That state proves protected source delivery and cross-surface disposition; it
does not waive feature-specific acceptance artifacts. When planning remains
held after merge, the audit retains a named gap with an owner and dependency
until the artifact is captured, and planning RELEASE remains forbidden.
Otherwise, planning RELEASE waits only for the row promotion. Every row also
records source evidence and gives every exposure surface one disposition:

- `exposed` includes evidence for the real implementation;
- `gap` names an owner and concrete dependency identifiers;
- `deferred` names an owner and the dependency that must clear first;
- `not_applicable` explains why the slice is not independently meaningful on
  that surface.

Every `exposed` surface also carries an `authority` descriptor recording what a
caller is allowed to do once admitted. This is a different question from the one
`docs/status/authority-navigation.json` answers: that file records which ledger
is authoritative for a subject, while this descriptor records the authority the
surface itself admits a caller under.

| `admission` | Meaning |
|---|---|
| `in_process` | The caller links the surface directly and holds whatever the API grants. No boundary admits it, so no profile and no refusal vocabulary apply. |
| `registered_writer` | The boundary registers a document writer under a named profile, with that profile's retained-byte ceilings and the conflict codes it can refuse with. |
| `read_only` | The operation cannot mutate a document, so admission carries no writer and no writer-scoped refusal. |
| `descriptor` | The surface publishes a description and admits no caller at all. |

A `registered_writer` row names its `writer_profile`, its `bounds`
(`max_transaction_retained_bytes` and `max_session_retained_bytes`, either of
which may be `null` for a deliberately unquotaed ceiling), and the
`refusal_codes` the boundary can turn an admitted caller away with. The other
three admissions carry `writer_profile: null`, `bounds: null`, and an empty
`refusal_codes`.

That vocabulary is not transcribed into the ledger. The checker parses
`tools/timeline/src/writer_profile.cpp` and
`tools/timeline/include/pulp/tools/timeline/writer_profile.hpp` — the same
definitions the CLI and MCP boundaries register writers from — so a row cannot
name a profile, a ceiling, or a refusal code those boundaries do not implement.
A source the checker cannot read is an error, never a skip.

Live Product A exposure has the strongest evidence bar. A row may say
`exposed` only when it points to a canonical capability definition, typed
operation definition, executor binding, grant-profile policy, CLI projection,
MCP projection, and an executable test. A generic timeline tool, public header,
or design-time manifest entry cannot substitute for any of those layers.

For every sequencer delivery slice:

1. Add or update its `pending` ledger row in the same change as the feature;
   every watched changed path must be covered by that row's `owned_paths`.
   A cross-module sequencer slice also carries the exact
   `Sequencer-Exposure: <row-id>` commit trailer so generic ownership paths are
   bound to that pending row.
   `owned_paths` and `evidence` are not the same declaration: `owned_paths`
   claims a file as the row's own and puts it under the watch, while `evidence`
   only cites a file as proof and does not. Cite a shared or foreign file as
   evidence; claim a file as owned only when this row is the single ledger
   owner of it, because a file two rows both claim is owned by neither.
2. Classify infrastructure and backend-only work explicitly instead of
   inventing a user control for it.
3. Preserve unresolved gaps with an owner and dependency; do not silently
   downgrade them to `not_applicable`.
4. After the protected merge, publish a ledger-only follow-up that promotes the
   row to `released` with exact merge evidence; only then release its planning
   claim.
5. When removing or superseding a row, add a `pending` tombstone with its claim
   ID in the removal change. Promote it to `released` with the protected merge
   SHA in the same ledger-only follow-up used for release evidence. Released
   tombstones are immutable; never reuse their IDs.
6. Validate the ledger and its calibrated negative controls:

   ```sh
   python3 tools/scripts/sequencer_exposure_check.py --base origin/main
   python3 tools/scripts/test_sequencer_exposure_check.py
   ```

The required version-and-skill workflow runs the same base-aware check. A
change under the watched timeline, playback, timeline CLI/MCP, test, skill,
documentation, or workflow surfaces fails if it omits the ledger. The checker
requires every changed watched path to belong to an added or materially changed
row, so an unrelated ledger edit cannot satisfy the gate. It also requires a
tombstone when a live row disappears and refuses changes to an existing
tombstone.

The ledger's top-level audit state is separate from row delivery. It stays
`in_progress` while any historical release is unreconciled or any program-wide
surface fact remains unresolved, and names the owner and dependencies for each
gap. A set of valid rows must not be presented as a complete census while that
audit state remains open.

## Author a target manifest

Ordinary `pulp_add_plugin` targets remain production-stripped unless they opt in.
The installed SDK ships the canonical Standalone host adapter. A controlled
target must build only the `Standalone` format, provide `PROCESSOR_FACTORY`, set
an explicit non-production `CONTROL_PROFILE`, and list its
`CONTROL_CAPABILITIES`. Other plugin formats and mixed-format declarations fail
closed instead of inheriting an endpoint. On macOS GPU builds the adapter
supports the full canonical Standalone capability set; other supported builds
currently accept only `dev.pulp.instance/read@1` and
`dev.pulp.state/read@1`. Configure-time diagnostics reject unavailable adapter
components or capabilities.

The declaration is an upper bound; it never grants a client by itself.
Mutation capabilities additionally require
`dev.pulp.session/control@1`. Runtime evaluation is accepted only under
`research-unsafe` with `ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL`; no named grant profile
automatically grants it.

The Pulp-owned host UI executor is a composition building block, not automatic
activation. It binds one registration/session/instance/publication and an
opaque view generation, uses the existing main-thread capture/input/evaluator
seams, strips ancillary PNG metadata, and publishes window or exact-node PNGs
as sensitive redacted evidence only through broker-owned artifact storage.
`ui.input` is a develop-only, controller-lease mutation: one
closed-schema pointer, keyboard, focus, or text event is dispatched per receipt
to the exact node named under that view generation. Pointer coordinates are
finite root coordinates bounded to +/-1,000,000; buttons, phases, key names,
target IDs, generation IDs, and UTF-8 text all have explicit bounds. The
executor rechecks authority/cancellation/deadline before and after dispatch and
binds retained pointer/focus state to the broker's opaque projected authority.
The installed-host composition retains the canonical authority-end
subscription and posts exact-owner release through the registered main-thread
RPC on revocation, expiry, client disconnect, or host teardown. Fenced teardown
reports failure if main-thread cleanup could not run.
Integrations must not substitute raw Inspector methods or generic host/port
discovery.

Runtime evaluation composition additionally requires the exact validated
registration and its digest-matched manifest, an interrupt-capable evaluator,
and an explicit host redactor. The request deadline is passed into the
evaluator, while broker cancellation or revocation interrupts in-flight work;
unredacted or oversized results fail closed.

Build profiles are artifact policy, not client grants:

| Build profile | Authoring rule |
|---|---|
| `production-stripped` | Default. Endpoint and capability list must be empty. |
| `developer-local` | Explicit developer capabilities; Standalone endpoint only. |
| `test-deterministic` | Explicit deterministic test/T0 capabilities; never a production default. |
| `support-diagnostics` | Explicit instance, state, GPU-health, diagnostics, or log reads only. |
| `research-unsafe` | Explicit research list; evaluation also needs the separate acknowledgement. |

Runtime grant profiles are a different, smaller vocabulary:
`inspect-readonly` (the `observe` set), `observe`, and `develop`. A grant request
is intersected with the exact live registration's declared capabilities. Empty
intersection, missing consent, stale publication, or a dead session denies the
request.

Interactive grants from the trusted Pulp CLI, host UI, or broker-owned prompt
keep their established reusable operation behavior except for GPU startup-health
reads. An interactive `dev.pulp.gpu/health.read@1` approval admits at most one
fresh idempotency identity; reconnecting or changing the request ID may only
replay that operation's existing durable receipt. Different params, operation
identity, target lineage, or idempotency identity require new consent. The
interactive decision ID itself remains single-use for every capability, and
runtime evaluation still requires interactive consent. An explicit existing-user
policy may authorize reusable GPU-health reads.

### GPU startup-health snapshots

`dev.pulp.gpu/health.read@1` is the exact-instance read operation for the
versioned `pulp.gpu-health-read-result.v1` response. It is a sensitive,
read-only background operation: calling it reads a bounded, already-produced
snapshot. It does not open an editor, render a frame, compile a shader, start a
trace, prewarm a cache, or run on the audio thread.

The response preserves the complete `pulp.gpu-health-result.v1` device and
render-health object under `health`. Its `startup` object binds the measurement
clock and explicit `native-compositor-presentation` or
`headless-capture-complete` endpoint, cold/warm trials, content and target
signatures, prepared/fallback state, adapter class, bounded event-loss fields,
and nullable GPU/Perfetto evidence IDs. The budget freezes separate nonzero
cold and warm trial counts whose sum and observed composition must match the
total trial count. A performance verdict or final
`queue-B4`, `queue-B4-investigation`, or `no-change` disposition is valid only
after the budget is ratified and the required correlated capture is lossless.
`dropped_event_count` and `truncated` describe capture integrity;
`missing_trace_categories` describes instrumentation coverage and may remain
nonempty for the plan-authorized investigation or passing no-change cases.
An unratified budget fixes how future trials will be interpreted but cannot
publish a performance pass or fail.

Once the exact live product advertises the operation, read it through the
existing generic control CLI:

```sh
pulp control call --instance "$INSTANCE_ID" dev.pulp.gpu/health.read@1 \
  --profile inspect-readonly --params '{}' --json
```

Registry and MCP presence do not imply host availability. Control-enabled
Standalone products now compose `ControlGpuHealthProvider` with a UI-thread
view adapter and advertise the operation only when that editor surface exists.
The adapter captures the host back buffer after ordinary event/render work,
records authentic `GpuSurface::adapter_info()` when present, applies the shared
PNG content floor, and publishes an immutable snapshot for the exact admitted
registration, instance, and publication. Reads are atomic and never render,
block, compile, trace, or touch the audio thread.

The default Standalone composition is intentionally conservative: capture
completion bounds back-buffer readiness but does not prove the native present
boundary. Visible Standalone, DAW, and Forge campaigns require independent
native compositor evidence. Only a provider explicitly configured for the
constrained headless role uses capture completion as its endpoint, and it cannot
claim compositor present timing. A trusted visible product composition may supply independent native
present timestamps, unique editor-lifecycle and observed cache-state identity,
direct submission evidence, bounded compile/upload/hidden-frame/present timings,
signatures, and the same 32-hex GPU evidence ID observed by the same-instance
trace. Capture completion never substitutes for presentation.
Without a ratified budget, exact lifecycle/cache identity, submission, target,
same-instance trace identity, and the role-appropriate endpoint, the versioned
`pulp.editor-first-visible.v1` startup result remains unverified. Missing causal
stage timings or source/shader identity are instead recorded as instrumentation
coverage gaps; they never become dropped-event claims.

Even a locally complete live snapshot is raw campaign evidence, not the A3
acceptance decision. The closed A3 verifier independently binds the ratified
budget and raw samples, product/host artifacts, exact trace and analyzer replay,
blank negative, and audio-thread exclusion receipt before any B4 disposition is
legal. `PULP_GPU_HEALTH_SEED_BLANK_FRAME=1` provides the deterministic blank
first-frame negative control. Timeout, instance loss, event loss, invalid PNG,
blank content, malformed producer data, reused lifecycle identity, and missing
adapter identity all fail closed. This runtime operation remains absent from the
design-time agent capability manifest.

## Vendor operations

A controlled Standalone target can expose its own operations next to the Pulp
ones. A vendor operation goes through the same broker, grants, consent,
controller leases, schema checks, and durable receipts as a `dev.pulp.*`
operation. It adds no transport, socket, or port of its own.

### Namespace rules

- A vendor id is a lowercase reverse-DNS name of two to eight labels, such as
  `com.acme` or `com.acme.synth`. Each label starts with a letter and holds
  only `a-z`, `0-9`, and `-`.
- The vendor id must equal the target's `BUNDLE_ID` or be one of its
  reverse-DNS prefixes. A target with bundle id `com.acme.synth` can declare
  vendor `com.acme` or `com.acme.synth`, never `org.other`.
- Pulp reserves `dev.pulp`, everything below it, and every vendor whose first
  label is `pulp`.
- An operation id is `dev.<vendor>/<name>@<version>`, for example
  `dev.com.acme/presets.load@1`. The name has one to six lowercase segments
  separated by `/` or `.`. The version is a positive integer without a leading
  zero.
- A changed contract gets a new version. Keep the old version declared while
  clients still call it.

### Declare operations

Write a declaration file and name it in `pulp_add_plugin`:

```cmake
pulp_add_plugin(AcmeSynth
    FORMATS Standalone
    BUNDLE_ID com.acme.synth
    PROCESSOR_FACTORY acme::create_processor
    CONTROL_PROFILE developer-local
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.session/control@1
        dev.pulp.vendor/read@1
        dev.pulp.vendor/gesture@1
    CONTROL_VENDOR_OPERATIONS control/vendor-operations.json
    CONTROL_VENDOR_SOURCES control/vendor_operations.cpp)
```

```json
{
  "schema": "dev.pulp.control/vendor-operations@1",
  "vendor": "com.acme",
  "operations": [
    {
      "id": "dev.com.acme/presets.count@1",
      "tier": "read",
      "description": "Count the presets in one bank.",
      "input_schema": {
        "type": "object", "additionalProperties": false,
        "properties": {"bank": {"type": "string", "maxLength": 32}}
      },
      "output_schema": {
        "type": "object", "additionalProperties": false, "required": ["count"],
        "properties": {"count": {"type": "integer", "minimum": 0}}
      }
    }
  ]
}
```

The file holds one to 64 operations and at most 128 KiB of printable ASCII.
Each operation needs `id`, `tier`, a `description`, and closed `input_schema`
and `output_schema` objects. Unknown fields are rejected. A description is one
line of 1 to 512 printable ASCII characters; line breaks, tabs, and other
control characters are refused, including when written as JSON escapes.

Schemas use a closed JSON Schema subset: `type`, `properties`, `required`,
`additionalProperties`, `items`, `minItems`, `maxItems`, `uniqueItems`,
`enum`, `const`, `minimum`, `maximum`, `minLength`, `maxLength`, `oneOf`,
`default` (a scalar, with a string default at most 512 bytes), and a root
`$schema` naming draft 2020-12. Both roots are objects.
Every object sets `additionalProperties` to `false`. Every string sets
`maxLength`. Every array sets `items` and `maxItems`. Schemas nest at most 16
deep. Configure fails on a missing file, a size or encoding error, or a tier
without its capability. The broker parses and validates the whole declaration
again at registration and rejects the registration if it is invalid.

### Tiers

Each operation declares one tier. Each tier is carried by one fixed Pulp
capability, so it is granted and consented like the Pulp operations of the
same risk.

| Tier | Capability | Use for | Grant |
|---|---|---|---|
| `read` | `dev.pulp.vendor/read@1` | No side effects | `inspect-readonly`, `observe`, or `develop` profile |
| `gesture` | `dev.pulp.vendor/gesture@1` | Reversible edits such as loading a preset | `develop` profile; needs the controller lease |
| `render` | `dev.pulp.vendor/render@1` | Work that spends CPU or memory, such as an offline render | Single-operation grant only |
| `destructive` | `dev.pulp.vendor/destructive@1` | Changes that cannot be undone, such as deleting a user preset | Single-operation grant only; needs the controller lease |

`CONTROL_CAPABILITIES` must list the capability of every tier the declaration
uses, and must not list a vendor tier that no operation uses. The `gesture` and
`destructive` tiers also need `dev.pulp.session/control@1`.

A single-operation grant names one operation id, and the broker confines the
grant to that id. Consenting to `dev.com.acme/notes.play@1` does not authorize
another render-tier operation of the same target. A profile grant covers every
operation of the tiers it contains.

### Bind handlers

The broker admits only declared operations. Code supplies the handlers through
a factory installed once per process, before the Standalone host starts. Put
that code in the files listed under `CONTROL_VENDOR_SOURCES`. They are built
only into the controlled Standalone and the separate host the broker launches,
never into other plugin formats. Install the factory from a namespace-scope
initializer:

```cpp
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/inspect/control_vendor_operation_executor.hpp>

std::shared_ptr<const pulp::inspect::ControlVendorOperationHandlers>
acme_vendor_operations(pulp::format::Processor& processor, pulp::state::StateStore& store) {
    auto handlers = std::make_shared<pulp::inspect::ControlVendorOperationHandlers>();
    handlers->add("dev.com.acme/presets.count@1", [&](const auto& call) {
        return pulp::inspect::ControlVendorOperationResult::success(R"({"count":3})");
    });
    return handlers;
}

namespace {
[[maybe_unused]] const bool acme_vendor_operations_installed =
    pulp::inspect::install_standalone_vendor_operations_factory(&acme_vendor_operations);
}
```

Handlers run on the host main thread, so they follow the main thread's rules:

- Finish within the call's deadline. A handler that outlives it leaves the
  receipt reporting that the outcome is unknown after it started.
- Never touch audio-thread data directly. Read and write parameters through
  `StateStore`, as `store.get_value()` and `store.set_value()` do, or through
  the processor's own real-time-safe paths.
- Never allocate on, lock against, or signal the audio thread.

The broker has already checked the
grant, consent, lease, and tier, and validated the arguments against the input
schema. A handler still treats argument values as untrusted data. Long handlers
call `still_authorized()` between steps and stop when it returns false. The
broker validates the result against the output schema before the receipt
completes. A handler bound to an undeclared id is dropped. A declared operation
without a handler fails as not implemented. An exception becomes an internal
failure.

### Agent tool kit

The agent tool kit is a set of ready-made vendor operations a plugin can opt
into instead of writing its own. Link `pulp::inspect-agent-toolkit` through
`CONTROL_VENDOR_SOURCES` (the link is added for you) and call
`add_control_agent_toolkit()` from the handler factory.

| Operation | Tier | Does |
|---|---|---|
| `presets.list` | `read` | Lists factory and user presets and the current preset |
| `presets.load` | `gesture` | Loads one preset by name and folder |
| `presets.save` | `destructive` | Saves the patch as a user preset, replacing one with the same name |
| `patch.schema` | `read` | Describes every parameter: id, name, unit, range, default, step |
| `patch.get` | `read` | Reads all parameter values or the listed ids |
| `patch.set` | `gesture` | Sets values, clamped to each range; one unknown id changes nothing |
| `patch.randomize` | `gesture` | Moves parameters toward seeded random values; bypass, reset, and trigger parameters are skipped |
| `notes.play` | `render` | Renders a note list with optional automation lanes offline and summarizes the audio |
| `render.analyze` | `render` | Renders silence, an impulse, a sine, or seeded noise through the plugin and summarizes the audio |

The preset operations need a `PresetManager`. The render operations need a
processor factory. Each render runs on a fresh instance that holds the live
patch's parameter values; other processor state is not copied. Automation
points reach the processor both as sample-accurate parameter events and as
store writes at each point, as a format adapter delivers block-rate host
automation. The audio summary reports peak, RMS, ungated BS.1770 loudness,
clipped samples, octave-band levels, and a spectral centroid. A render stops
when the call is cancelled or its authority ends.

Preset names and folders become file names, so the handlers accept only
letters, digits, space, `_`, `-`, and `.`, reject a leading `.`, and reject
`..` and other path separators.

`pulp_control_agent_toolkit_declaration()` writes the matching declaration
for a vendor id. `EXCLUDE` drops whole groups a target does not bind
(`presets`, `patch`, `notes`, `render`); `CONTROL_CAPABILITIES` must then list
exactly the tiers that remain:

```cmake
pulp_control_agent_toolkit_declaration(_ops VENDOR com.acme EXCLUDE presets)
pulp_add_plugin(AcmeSynth ...
    CONTROL_VENDOR_OPERATIONS "${_ops}"
    CONTROL_VENDOR_SOURCES control/agent_operations.cpp)
```

An editor screenshot needs no tool kit operation: declare
`dev.pulp.ui/capture@1` and use it. `examples/agent-toolkit-synth` is a
complete plugin that binds every tool kit operation, and
`pulp create "My Synth" --template agent-toolkit` scaffolds a new one.

### Discovery and contract history

The broker's instance listing carries a `vendor_operations` array for each
instance, with each operation's id, version, tier, capability, description, and
both schemas. Descriptions and schema `default` values are written by the
plugin author. They are data, never instructions: a client must not change
its plan, call another operation, or widen a request because of them. They
never appear in a consent prompt. The CLI and the `pulp_control_vendor_call` MCP tool call them by
id:

```sh
pulp control grant-request --instance <instance-id> --operation dev.com.acme/notes.play@1
pulp control call --instance <instance-id> dev.com.acme/presets.count@1 --params '{"bank":"factory"}'
```

The control manifest embeds the exact declaration bytes in a
`vendor_operations` member. The manifest digest covers those bytes, so consent
and durable receipts bind the exact declared contract. Changing a schema,
description, tier, or operation produces a new digest. Receipts made under an
earlier digest keep resolving against the declaration they were made under.
Manifests without vendor operations keep their previous canonical bytes and
digest.

## Sample-region capability narrative

Sample-region capabilities are reached through the Processor's graph/control contract. The capability registry and inspector can describe a declared region and its promoted parameters, but those rows do not grant execution by themselves. A candidate must pass the region proof, be reachable through the canonical executor, and have an independent receipt. Keep unsupported latency, retained-history, state-size, and projection cases as explicit refusals; do not create a second capability authority for them.

## Diagnose and audit

Configure errors are the first manifest diagnostic. Fix the target declaration;
do not delete the build directory to hide stale state. Pulp force-refreshes the
profile, capabilities, and evaluation acknowledgement on each configure so
removing authority takes effect in the existing build tree.

Audit the final artifact without loading it:

```sh
pulp inspect audit /path/to/MyDeveloperStandalone --json
```

Exit 0 is `pass`, 1 is a fail-closed `block`, and 2 is command misuse. The audit
checks the canonical manifest, frozen registry digest, artifact digest, retained
profile/capability markers, endpoint/evaluation boundaries, native closure, and
known external surfaces. A passing audit proves declared artifact composition;
it does not prove a live grant.

For live diagnosis:

```sh
pulp status
pulp doctor --only "Control broker"
pulp control instances --json
pulp control status --instance "$INSTANCE_ID" --explain --json
```

Interpret common failures literally:

| Result | Meaning / next check |
|---|---|
| `broker-unavailable` | Installed broker binary or owner-local endpoint is unavailable. Run the broker doctor row. |
| `instance-not-found` / `not-found` | The exact instance is not live. Refresh inventory; never substitute a similar/newest instance. |
| `ambiguous-instance` | Inventory is invalid for exact selection. Stop and investigate rather than guessing. |
| `capability-unavailable` | The live registration did not declare any capability in the requested grant profile. Rebuild/relaunch the intended artifact. |
| `consent-required` | Broker policy needs a trusted decision. CLI/MCP UI acknowledgement is not authority. |
| `permission-denied` or stale-grant errors | Re-read status and request a new bounded grant for the current publication; do not retry an ambiguous mutation. |
| `unknown-needs-refresh` | Work may have applied after the response deadline. Refresh state/receipt before deciding whether to act again. |

## Enable, disable, and revoke safely

1. Use a trusted host integration that composes the canonical host-side adapter;
   ordinary `pulp_add_plugin` Standalones remain stripped until that adapter ships.
2. Select the narrowest build profile and exact capability list in that integration.
3. Run the offline artifact audit and review every declared capability.
4. Launch through the trusted Pulp integration; copy the exact `instance_id`
   from `pulp control instances --json`. On a clean SDK installation that
   command launches the broker-owned ordinary Standalone host when inventory is
   empty; author-specific hosts still launch only through their trusted Pulp
   integration.
5. Start read-only with `inspect-readonly`. Request `develop` only for an
   intended mutation and review the broker's consent prompt.
6. Revoke the returned grant with `pulp control revoke --grant ID`.
7. To disable future authority, remove the target's control declaration (or
   restore `production-stripped`), reconfigure, rebuild, and audit again. Stop
   the old live instance; a rebuilt artifact does not retroactively change an
   already running process.

Runnable paired T0/T1 CLI and MCP examples live in
`examples/capability-control/` and install under
`share/pulp/capability-control/`. They are generated from one corpus so the two
client surfaces cannot silently teach different operations.

See also the [threat model](../policies/capability-control-threat-model.md) and
[shipping guide](../guides/development-inspector-shipping.md).
