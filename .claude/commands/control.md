---
name: control
description: Discover and use Pulp's exact-instance typed runtime-control operations safely
---

Use the installed `pulp` command and the canonical broker. Start with read-only
discovery; never guess an instance, choose "newest", or add a host/port/raw
protocol fallback.

```bash
pulp control profiles --json
pulp control instances --json
pulp control status --instance "$INSTANCE_ID" --explain --json
```

For a typed operation, inspect the exact live instance and request only that
operation. Do not substitute a design-time agent capability row for runtime
authority.

```bash
pulp control grant-request --instance "$INSTANCE_ID" \
  --operation dev.pulp.gpu/health.read@1 --json
pulp control call --instance "$INSTANCE_ID" \
  dev.pulp.gpu/health.read@1 --params '{}' --json
```

Use `watch` only when the operation's registry contract permits it. Preserve
the returned registration, publication, instance, grant, request, idempotency,
and receipt identities in evidence. A GPU-health response is one bounded
runtime snapshot; it is not by itself an A3 campaign or terminal disposition.

Ask before requesting broader reusable grants, invoking mutations, revoking a
grant, or starting/stopping a trace. Use `pulp control audit PATH --json` for
offline artifact audit. If the broker or typed operation is unavailable, report
that state; do not recreate the retired Inspector transport.
