# Timeline SDK consumer

This is a standalone installed-SDK example. It is intentionally not added by
the repository's `examples/CMakeLists.txt`: configure it out of tree against a
staged or released Pulp SDK.

```sh
cmake -S examples/timeline-sdk-consumer -B build/timeline-sdk-consumer \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/timeline-sdk-consumer
./build/timeline-sdk-consumer/pulp-timeline-sdk-consumer
```

Its configure step audits the complete target closure—ten first-party engine
libraries and six runtime support archives—and rejects GPU, view,
format-adapter, graph, standalone, and plugin-host dependencies.
