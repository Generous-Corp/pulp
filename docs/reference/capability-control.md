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

## Author a target manifest

Control is stripped unless a target declares a profile and its exact stable
capability IDs:

```cmake
pulp_add_plugin(MyDeveloperStandalone
    FORMATS Standalone
    BUNDLE_ID dev.example.my-developer-standalone
    CONTROL_PROFILE developer-local
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.state/read@1)
```

The declaration is an upper bound. It creates a canonical artifact manifest and
retained shipping markers, but it does not activate an endpoint or grant a
client. Mutation capabilities additionally require
`dev.pulp.session/control@1`. Runtime evaluation is accepted only under
`research-unsafe` with `ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL`; no named grant profile
automatically grants it.

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

1. Add the narrowest build profile and exact capability list.
2. Reconfigure and build the intended Standalone artifact.
3. Run the offline artifact audit and review every declared capability.
4. Launch through the trusted Pulp integration; copy the exact `instance_id`
   from `pulp control instances --json`.
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
