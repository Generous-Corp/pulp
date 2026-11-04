# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the existing public
latency queries. It constructs a diamond graph with a custom four-sample delay,
checks total and node latency, and checks the planted unknown-node negative.
It does not implement a new runtime or claim a general latency/PDC map, CLI/MCP
serialization, realtime safety, or full G-7 completion.

State: PARTIAL; local commit `0dea423d3af6dbd8f5814694ff41eb757232501f` is clean. `git diff --check` passes. A raw source-tree syntax check is blocked because this checkout has no external `choc/audio/choc_MIDI.h`; the required installed-SDK CMake consumer build and runtime receipt are pending. No PR has been opened and no full G-7 completion is claimed.
