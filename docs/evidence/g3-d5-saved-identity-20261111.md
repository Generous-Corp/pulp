# G3 D5 saved-document inspection — bounded admission slice

This slice adds a data-only `GraphSerializer::inspect_document` surface. It
accepts an explicit `pulp.graph.authored-io-gain.v1` document or the reserved
`pulp.graph.saved-identity.v6` envelope and returns owned canonical bytes plus
a domain-separated SHA-256 digest. It never loads plugins, invokes factories,
restores Processor state, prepares a graph, or writes a file.

Inspection is fail-closed for empty/oversized input, non-object roots,
malformed or future format versions, legacy documents without logical
identity, unsupported profiles, duplicate keys, non-finite numbers, and
excessive nesting. A string/escape-aware linear pre-scan rejects depth beyond
64 before the recursive JSON parser runs, so very deep hostile input cannot
drive parser recursion. Existing v4 graph loading/migration and D1/D2/D4
bytes are unchanged.

Focused controls are in `test/test_graph_serializer.cpp` under `[g3][d5]`:
object-member reorder canonicalization, duplicate/legacy/future/profile
refusals, nesting bounds, and the v6 envelope. The public consumer in
`examples/g3-resolved-identity-consumer` exercises the inspection API before
the existing resolved-plan receipt path.

## Receipts

- `pulp build --all` completed successfully on the pinned worktree.
- `./build/test/pulp-test-group-app-host '[g3][d5]' --reporter compact`: 3
  cases, 21 assertions passed, including a 200,000-bracket hostile input and
  brackets inside a JSON string.
- `python3 test/test_g3_io_gain_identity.py`: 10 tests passed.
- Installed SDK consumer: `cmake --install build --prefix build/g3-sdk`,
  external configure/build, and `ctest --test-dir build/g3-installed`
  passed 1/1. The consumer executable is 4,944,688 bytes with SHA-256
  `db5249382b0ad61745be5f31beef78105bc30386e3a93583b504174cc8ffa433`.
- `git diff --check` passed. `tools/scripts/gates.sh origin/main` ran all
  host-available checks; the dedicated-builder host refused the source-only
  build-gate suites and the script-input manifest check reported NOT CHECKED.
- Mutation proof: `tools/scripts/confirm_failure.sh` on a clean tracked source
  file, targeting `pulp-test-group-app-host '[g3][d5]'`. The planted mutation
  omitted the required embedded-NUL separator; baseline PASS, broken FAIL, and
  restored PASS were all observed. The harness reported `CONFIRMED` after
  recompiling `core/host/src/graph_serializer.cpp` in each phase.
- Pre-scan mutation proof: the same harness replaced the depth guard with a
  no-op. Baseline PASS, planted deep-input failure, and restored PASS were
  `CONFIRMED`; `graph_serializer.cpp` recompiled in all three phases.
