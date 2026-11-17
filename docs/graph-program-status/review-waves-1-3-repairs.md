# Adversarial review of graph waves 1-3: host repairs (2026-11-17)

Scope: the confirmed should-fix findings and cheap notes from the second-model
review of the merged graph PRs, except the host timeline binding findings,
which are repaired separately, and the MCP diff and AU v2 timing findings,
which are in `review-waves-1-3-repairs-mcp-au.md`. Each finding was re-read
against `origin/main` and reproduced by a test that failed before the fix.

## Fixed

| Finding | Repair | Test |
|---|---|---|
| Isolated CLAP launch: the overall session deadline (5 s) overrode the separate load deadline, so a 3 s hello plus a 3 s load was killed | `launch_exchange_deadline` gives hello and the load report each their own 5 s budget, both inside a 10 s session (`kLaunchSessionBudget`); both transports use it | `Isolated CLAP launch gives hello and the load report each a full budget` (`pulp-test-isolated-clap-protocol`) |
| Sibling continuity could pass with the sibling silent in both runs | the reference sibling is checked against its own oracle (gain 0.25 of the previous block's input), in the POSIX and Windows suites | `Isolated CLAP killed worker keeps in-process sibling bit-exact` and its Windows twin |
| GPU admission proof passed on a host that executed no GPU work, carried by the manifest-defect case alone | that case renders and calls `skip_unless_gpu_executed` like the others, so the whole proof skips (Catch exit 4) on such a host | reproduced by forcing the GPU-execution check to skip: before, 1 passed and 4 skipped with exit 0; after, 5 skipped with exit 4 |
| `add_spectral_asset_source_v1` added its node without an authored ID | it goes through `push_node_locked_` like every other node | `Asset source and morph nodes carry a minted authored ID like every other node` |

## Proven only on macOS

The Windows launch path shares `launch_exchange_deadline` and
`kLaunchSessionBudget` with the POSIX path and compiles the same call shape, but
only the Windows Server VM can run it and the Windows sibling test. Owner-run
commands are in `docs/guides/isolated-clap-worker.md`.

## Left open

- Custom node types that set `create` without `destroy`: the state-acceptance
  probe never frees its instance, but neither does any node instance of such a
  type (`make_custom_instance` has the same shape). Requiring `destroy` when
  `create` is set changes public registration behavior, so it needs an owner
  decision rather than a review repair.
