# G3 D4 prepared export consumer

This standalone project uses only the installed `Pulp::host` target. It binds,
prepares, commits, and exports the admitted empty serial graph profile without
including any source-tree or private headers.

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/pulp/lib/cmake/Pulp
cmake --build build --target pulp-g3-d4-prepared-export-consumer
ctest --test-dir build --output-on-failure
```
