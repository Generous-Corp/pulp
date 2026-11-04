# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the existing public
latency queries. It constructs a diamond graph with a custom four-sample delay,
checks total and node latency, and checks the planted unknown-node negative.
It does not implement a new runtime or claim a general latency/PDC map, CLI/MCP
serialization, realtime safety, or full G-7 completion.

State: IN PROGRESS; exact tests and installed-SDK build pending.
