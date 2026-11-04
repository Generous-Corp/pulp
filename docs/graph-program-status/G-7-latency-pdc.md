# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the existing public
latency queries. It constructs a diamond graph with a custom four-sample delay,
checks total latency and the delayed node's input latency, and checks the planted
unknown-node negative.
It does not implement a new runtime or claim a general latency/PDC map, CLI/MCP
serialization, realtime safety, or full G-7 completion.

State: PARTIAL; the installed SDK `1.6.0` (provenance source tag `v1.6.0`, SHA `c02bf6f190eb3a8d7e3c9883aa816f2f5a09be38`) configured and built this consumer. CTest initially exposed a semantic mistake in the fixture: `node_latency_samples` is input-arrival latency, not intrinsic latency. The fixture was repaired to assert total latency 4 and node-input latency 0; the repaired CTest rerun is pending. This SDK is not claimed as exact current-main provenance, so no PR or full G-7 completion is claimed yet.
