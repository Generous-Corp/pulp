---
name: ci-host
description: Onboard a Mac as a Tart-VM CI host (optional; wraps setup-ci-host.sh)
---

`pulp ci-host` is the optional, discoverable wrapper around
`tools/ci/setup-ci-host.sh` for turning a Mac into a Tart-VM CI host. It is an
**advanced/contributor path — never required**, and Shipyard stays
encouraged-not-mandated. The real work lives in the script; the command just
makes it discoverable and passes flags through.

## Setup a host

```bash
pulp ci-host setup --class <m5|studio|macbook|...>          # minimum
pulp ci-host setup --class m5 --copy-from 'macstudio:/Volumes/Workshop/VMs/vms/pulp-build-runner:latest'
pulp ci-host setup --class m5 --validate                    # also run a one-shot VM build to prove it
```

Common flags (all forwarded to `setup-ci-host.sh`):

- `--class <name>` — **required** host class for the runner label (`m5`, `studio`, `macbook`, …)
- `--copy-from <ssh:path | path>` — rsync a golden in from another host/drive (sparse-safe)
- `--validate` — after setup, run one ephemeral VM build on the host-only label
- `--no-agent` — do everything except install/load the launchd agent

Run `pulp ci-host setup --help` for the full flag list.

## Notes

- Runs from inside a Pulp checkout (it resolves `tools/ci/setup-ci-host.sh`).
- For the from-scratch host recipe and gotchas, see
  `docs/guides/mac-ci-host-setup.md` and the `tart-ci` skill.
- The bare-metal runner lane is a different tool; this command is the Tart-VM
  (ephemeral per-job) host path.
