# Dawn Phase E direct executable receipt (2026-10-02)

> **Historical boundary note:** the earlier aggregate `NOT_BUILT` and
> “adapter absent” wording is superseded by the direct executable results in
> this receipt. The live adapter probe and hardware submission remain outside
> this receipt's claim.

This receipt records direct execution of the built private binaries. It does not
run the aggregate CTest group, which contains unrelated intentional
`NOT_BUILT` controls.

## Build targets

Built with the governed command:

```text
./build/pulp build --target pulp-test-gpu-shared-io-convolution-pipeline \
  --target pulp-test-gpu-shared-io-convolution-session \
  --target pulp-test-gpu-shared-io-slot-ledger
```

The resulting executables were:

* `build/test/pulp-test-gpu-shared-io-convolution-pipeline`
* `build/test/pulp-test-gpu-shared-io-convolution-session`
* `build/test/pulp-test-gpu-shared-io-slot-ledger`

## Direct pass/fail results

Every command exited **0**:

| Executable and filter | Result |
| --- | ---: |
| `pulp-test-gpu-shared-io-convolution-pipeline '[pipeline]'` | exit 0; 9 cases, 171 assertions |
| `pulp-test-gpu-shared-io-convolution-session '[recovery]'` | exit 0; 6 cases, 95 assertions |
| `pulp-test-gpu-shared-io-convolution-session '[ordering]'` | exit 0; 2 cases, 23 assertions |
| `pulp-test-gpu-shared-io-slot-ledger '[deadline]'` | exit 0; 2 cases, 42 assertions |
| `pulp-test-gpu-shared-io-slot-ledger '[retirement]'` | exit 0; 5 cases, 85 assertions |
| `pulp-test-gpu-shared-io-slot-ledger '[completion]'` | exit 0; 6 cases, 108 assertions |

The exact adversarial cases were also run directly and exited **0**:

| Case | Target | Result |
| --- | --- | ---: |
| `shared session loss retires exact work but never reprimes the lost provider` | `pulp-test-gpu-shared-io-convolution-session` | exit 0; 1 case, 12 assertions |
| `shared convolution session does not let an out-of-order terminal leapfrog its head` | `pulp-test-gpu-shared-io-convolution-session` | exit 0; 1 case, 12 assertions |
| `shared IO arena expiry retains terminal credit and allocation until late retirement` | `pulp-test-gpu-shared-io-slot-ledger` | exit 0; 1 case, 23 assertions |

The full three binaries were previously run directly as well: pipeline 9/9,
session 18/18, and ledger 36/36 passed.

## Evidence covered

* Forced provider-loss handling fences the session, retires exact in-flight work,
  and refuses to reopen the lost provider.
* Out-of-order/late terminal publication cannot leapfrog the chronological head.
* A late retirement retains terminal credit and backing storage until physical
  completion, preventing premature slot reuse.
* Recovery, ordering, deadline, retirement, and completion filters all pass
  without aggregate-group controls.

## Boundary and remaining limitation

No public SDK header, GraphNode field, or ABI changed. These are private
shared-I/O lifecycle proofs using the built pipeline/session/ledger targets.
The live Dawn adapter probe (`pulp-gpu-shared-io-private-convolution-probe`) is
still absent from this build's Ninja graph, so this receipt does not claim
hardware Dawn submission or adapter-level device-loss injection. The next
required action is a Dawn-capable configure/build followed by that probe's
baseline and forced-loss/late-completion scenarios.
