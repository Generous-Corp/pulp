# Pulp authored I/O and Gain identity

This installable package exposes the closed G-3 D1/D2 semantic profile for
explicit `pulp.graph.authored-io-gain.v1` documents. It does not read legacy
`.pulpgraph` files or prepare, load, scan, execute, or modify a graph.

Install with Python 3.13 using `python3 -m pip install tools/graph`. Python
3.13 supplies the pinned Unicode 15.1.0 normalization table; other versions
fail closed. The public Python API is
`pulp_graph_identity.parse_document`, `manifest`, `fingerprint`, and `diff`.
The installed CLI is `pulp-graph-identity fingerprint DOCUMENT --json` and
`pulp-graph-identity diff BEFORE AFTER --json`. Diff exits 0 for equal, 1 for
different, and 2 for invalid or unavailable operands. The CLI prints one JSON
result to stdout.

The profile and frozen vectors are specified in the G-3 D1/D2 planning contract.
Resolved plan identity, aliases, legacy migration, and prepared export are
outside this package's closed input profile.
