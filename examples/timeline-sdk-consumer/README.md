# Timeline SDK consumer

This is a standalone installed-SDK example. It is intentionally not added by
the repository's `examples/CMakeLists.txt`: configure it out of tree against a
staged or released Pulp SDK.

```sh
cmake -S examples/timeline-sdk-consumer -B build/timeline-sdk-consumer \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/timeline-sdk-consumer
./build/timeline-sdk-consumer/pulp-timeline-sdk-consumer
./build/timeline-sdk-consumer/pulp-timeline-cookbook-consumer
./build/timeline-sdk-consumer/pulp-dawproject-import-sdk-consumer
```

Its configure step audits both the engine target closure and the optional
DAWproject importer closure, rejecting GPU, view, format-adapter, graph,
standalone, and plugin-host dependencies. The cookbook executable exercises
project construction, checked transaction/undo/redo, scene authoring,
compilation, one render block, and capture preparation. It does not open an
audio device or seal captured audio into a take. The importer executable proves
that the installed importer header and implementation link and run downstream;
it is a link probe, not a representative import walkthrough. The public
[Timeline SDK guide](../../docs/guides/timeline-sdk.md#optional-dawproject-importer)
documents the supported and rejected DAWproject features, and
`test/fixtures/timeline/dawproject/linear_subset.dawproject.xml` is the
representative supported `project.xml`.
