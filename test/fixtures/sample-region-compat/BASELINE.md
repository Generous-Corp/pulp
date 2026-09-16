# C0 baseline provenance

- Pulp source: `e922ba8e7e03742cdb0b748c1bc872ff18ecab7c`
- Immutable sample-region brief revision:
  `70da8f21f8d533f1a4a20a9cd308c4d867dbc1d8`
- Plan revision: `07017ab490b6ea0cd1e7be77c6ebee2cdb5f4058dcbfceee4d21a544496afb08`
- Baseline host: `darwin-arm64`
- Retained installed SDK used for the old-consumer compile:
  `0.837.0`, source `6914d57d4d400e93bc85b8a0d7b9567943735769`
- Installed SDK provenance SHA-256:
  `127b106fc710ba13e0a3c67038512ca98682c07f972cc064961c6812b1fa65b4`
- Installed SDK consumed-input receipt:
  `installed-sdk-0.837.0.json` (the provenance record plus every header,
  CMake input, static library, and dylib used by the old-source consumer).

The expected files were emitted once from the Pulp source above and then
committed. The normal verifier never regenerates them.
