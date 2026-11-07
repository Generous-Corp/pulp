# A9 agent design loop consumer

This standalone consumer is the bounded A9 flagship slice. It uses only the
installed `Pulp::host` target and follows the checked-in, pinned
`fixtures/action-plan.json`:

1. discover and audit the public catalog;
2. author an input → gain → output graph and save its `.pulpgraph` notation;
3. reload and validate the saved graph with plugin loading disabled;
4. render it in three fixed blocks, attach an audio-edge probe, and compare the
   probed render with the baseline;
5. apply one bounded gain edit and prove the render changes;
6. reload the original saved input and reproduce the baseline render exactly.

The plan pins a deterministic offline action model, this tool version, and the
audited source SHA. The executable parses the JSON with duplicate-key
rejection, then executes the ordered plan steps and records each completed
step. It declares `network_policy: deny` and
`plugin_policy: deny-ambient`; the executable rejects URLs, allow policies,
missing steps, and missing mutation declarations before opening a graph. A
JSONL action log records the plan digest, every action result, the denial
policies, and a topology-only semantic fingerprint. Parameter edits must
change samples while preserving that fingerprint, and replay must restore the
original samples and fingerprint.

Configure it against an installed SDK:

```sh
cmake -S examples/graph-agent-loop -B build/graph-agent-loop \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/graph-agent-loop
ctest --test-dir build/graph-agent-loop --output-on-failure
```

The CTest suite includes three planted negative plans (`network-allowed`,
`plugin-allowed`, and `missing-action`). Each is a full copy of the positive
plan with one field or step changed. CTest asserts the named denial rule and
marks each test `WILL_FAIL`; a green positive run without those negatives is not
an admission receipt.

The fixture is hermetic: it disables plugin loading and default plugin roots
while reloading the saved graph. It does not claim realtime scheduling, model
or network access, arbitrary plugin hosting, or the complete A9/A10 flagship.
The generated baseline and edited graph files are disposable action-plan
artifacts; the executable's final line is the reproducible machine-readable
receipt.

