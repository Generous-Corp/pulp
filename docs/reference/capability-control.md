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

## Author a target manifest

Ordinary `pulp_add_plugin` targets are currently production-stripped. The
canonical broker, protocol, manifests, grants, and trusted-host launch path are
available, but the dedicated host-side adapter that binds a general
`pulp::standalone` processor and state store is not yet shipped. Consequently,
`pulp_add_plugin(... CONTROL_CAPABILITIES ...)` fails at configure time instead
of producing an artifact that falsely claims an endpoint.

The frozen declaration syntax will become an upper bound when that adapter is
available; it will never activate an endpoint or grant a client by itself.
Mutation capabilities additionally require
`dev.pulp.session/control@1`. Runtime evaluation is accepted only under
`research-unsafe` with `ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL`; no named grant profile
automatically grants it.

The Pulp-owned host UI executor is a composition building block, not automatic
activation. It binds one registration/session/instance/publication and an
opaque view generation, uses the existing main-thread capture/input/evaluator
seams, and publishes window or exact-node PNGs only through broker-owned
artifact storage. `ui.input` is a develop-only, controller-lease mutation: one
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
| `support-diagnostics` | Explicit instance, state, diagnostics, or log reads only. |
| `research-unsafe` | Explicit research list; evaluation also needs the separate acknowledgement. |

Runtime grant profiles are a different, smaller vocabulary:
`inspect-readonly` (the `observe` set), `observe`, and `develop`. A grant request
is intersected with the exact live registration's declared capabilities. Empty
intersection, missing consent, stale publication, or a dead session denies the
request.

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
