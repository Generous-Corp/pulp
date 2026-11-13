# Live retuning in an editable instrument rack

Two graph-owned `Processor` instruments receive one keyboard note-on. A host
control thread resolves tuning and changes `voice.frequency` at samples 8191
and 24577, while the sounding instrument keeps its oscillator phase and ADSR
state. The sibling instrument remains at 330 Hz.

Use a clean Release source-install SDK containing `host`, `midi`, `signal`
and `audio-analysis`. Optional Scala support enables the real local-scale
render as well. This project includes only installed public Pulp headers.

```sh
cmake -S examples/g11-live-retune-consumer -B build-g11-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/to/sdk \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
tools/ci/governed-build.sh cmake --build build-g11-consumer
ctest --test-dir build-g11-consumer --output-on-failure --no-tests=error
build-g11-consumer/pulp-g11-live-retune --artifacts build-g11-consumer/audio
```

The positive case checks numeric phase, envelope, sample chronology, rendered
pitch, refusal preservation and bit-identical legal partitions. Three planted
defects and a separately compiled missing-write mutation must be detected by
the unchanged oracle. See the [cookbook](../../docs/guides/g11-live-retune-cookbook.md)
for the contract and the explicit remaining G-11 obligations.

Boundary selection and thread joins belong to this offline test host. They
do not provide arbitrary sample scheduling, a runtime executor, or realtime
qualification. `tuning_control.hpp` is product-owned glue around Pulp's
existing provider/catalog/store interfaces.

## Notation

`graph.pat` spells this fixture's graph in SignalGraph patch notation as far as
the notation profile allows. It cannot be accepted: the fixture's nodes are
application code or constructs outside the default catalog, so the real
validator refuses it, and that refusal is the recorded result:

```sh
pulp graph validate graph.pat --json
# exit 1, "code": "UnsupportedContract", "rule": "unregistered type/version '<id>'"
```

The `g11-live-retune-consumer-notation-refusal` ctest runs the validator and pins the exit code,
code, node and type. See docs/guides/graph-patch-notation.md for the profile.
