# D6a: spectral notation under `# flow 2`

Status: first slice implemented; JSON persistence (D6) is a separate item.

## Delivered

- `# flow 2` header. Flow 1 documents parse exactly as before: argument
  handling runs only under the flow 2 header, and the formatter writes
  `# flow 1` for any graph that needs no flow 2 spelling.
- Constructor arguments (`core/host/src/graph_patch_constructors.cpp`):
  `pulp/audio_in` / `pulp/audio_out (channels C)` for 1 to 16 channels, and
  `pulp/spectral_analysis`, `pulp/spectral_synthesis`,
  `pulp/spectral_stage (... kind identity)` with `fft`, `hop`, `channels` and a
  required `rate`.
- One sample rate per document (`SampleRateMismatch`); `prepare()` at another
  rate fails with the spectral error `IdentityMismatch`.
- New diagnostic codes `SampleRateMismatch` and `UnsupportedChannelLayout`,
  spelled by `graph_patch_code_name` and passed through unchanged by the CLI
  and MCP `pulp_graph` tool.
- Formatter refusals with the payload size for real-mask stages, and refusals
  for codec asset nodes, in parser and formatter.
- Catalog rows: spectral analysis, stage and synthesis report
  `notation_state: flow_2_arguments` and their `flow_2_arguments`; boundaries
  list `channels`; the asset morph stays `unsupported_typed_api_only`.

## Evidence

- `test/test_graph_patch.cpp` (`[flow-2]`, `[flow-1-compat]`): parse, format,
  parse equivalence with exact rate bits; an identity render oracle through the
  parsed graph; prepare at the wrong rate; negative controls for a missing
  rate, a mismatched rate, unsupported layouts, argument grammar, mask and
  asset payloads, each paired with an admitted control.
- `test/test_cli_graph_code_parity.cpp`: CLI `validate`/`inspect` codes and
  `fmt` text equal the C++ results for flow 1 and flow 2 documents.
- `test/test_graph_patch_tools.py` (run by `cmake-graph-patch-sdk-consumer`):
  installed API, CLI and MCP agree on four flow 2 fixtures and on `fmt`.

## Remainder

- D6 JSON persistence: saved-graph format for analysis graphs (identity, exact
  rate bits, typed edges, stage kind and gains) through the D1 v7 envelope.
  Until it lands, `pulp graph save` refuses a flow 2 analysis graph with the
  serializer's reason.
- Mask gains in notation are not planned: masks stay in JSON.
- Asset morph and asset source nodes stay in JSON.
- Bus-layout boundaries (named channel sets) have no spelling.
- Live graph-file reload of a flow 2 spectral document is not claimed; it uses
  the same parser but no reload test covers a spectral graph.
