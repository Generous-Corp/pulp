# A9 agent design loop consumer

This standalone consumer is the bounded A9 flagship slice. It uses only the
installed `Pulp::host` target and follows one deterministic action plan:

1. discover and audit the public catalog;
2. author an input → gain → output graph and save its `.pulpgraph` notation;
3. reload and validate the saved graph with plugin loading disabled;
4. render it in three fixed blocks, attach an audio-edge probe, and compare the
   probed render with the baseline;
5. apply one bounded gain edit and prove the render changes;
6. reload the original saved input and reproduce the baseline render exactly.

Configure it against an installed SDK:

```sh
cmake -S examples/graph-agent-loop -B build/graph-agent-loop \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/graph-agent-loop
ctest --test-dir build/graph-agent-loop --output-on-failure
```

The fixture is hermetic: it disables plugin loading and default plugin roots
while reloading the saved graph. It does not claim realtime scheduling, model
or network access, arbitrary plugin hosting, or the complete A9/A10 flagship.
The generated baseline and edited graph files are disposable action-plan
artifacts; the executable's final line is the reproducible machine-readable
receipt.

