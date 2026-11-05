# Processor restore batch consumer

This standalone installed-SDK consumer registers a product Processor and restores
nondefault StateStore parameters plus plugin-owned bytes into fresh instances.
It renders them through SignalGraph and checks an exact arithmetic oracle.

The registry is a control-thread building block for hosts applying a collection
of already-decoded states to a separate candidate. It validates every record and
permission before executing a factory, constructs the entire batch before any
restore, and exports no candidates if either payload is rejected. Application
factories are trusted code and must return newly created, unprepared instances;
they must not expose instances to another thread or mutate live state themselves.
The exclusive-ownership checks reject shared aliases before restoration.
The outer envelope version and embedded StateStore schema are independent;
both require exact matches against their respective descriptor/store versions.
The restore path never invokes parameter migrations.

The consumer also proves bounded `.pulpgraph` v4 Processor emission and
transactional deserialize round-trip. State capture/coherence, JSON and base64
validation, migrations, graph edge/region validation, preparation, and atomic
publication remain separate work. No CLI/MCP loadability is claimed.

Copy this directory outside the Pulp checkout, install a Release SDK containing
the change, and configure with its package directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR="$PULP_SDK_PREFIX/lib/cmake/Pulp"
"$PULP_SOURCE_DIR/tools/ci/governed-build.sh" cmake --build build
ctest --test-dir build --output-on-failure
```

Pulp registers `cmake-g2-processor-restore-sdk-consumer` to install the SDK,
build this same consumer in a separate CMake project, and run its CTest.
It checks unknown/disallowed and duplicate types, exact type/state versions,
decoded per-part/aggregate/node limits, duplicate node ids, declared and actual
ports/capabilities, parameter rejection, custom rejection/throw, aliases to a
live node or earlier candidate, and late failure isolation. The live node's
parameter bytes, custom bytes, and rendered output must remain unchanged.
