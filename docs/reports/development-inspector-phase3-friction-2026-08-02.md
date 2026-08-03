# Development Inspector Phase 3 friction benchmark

Date: 2026-08-02

The benchmark used a fresh `cmake --install` prefix under `/tmp`, not binaries
resolved from the checkout. The prefix contained the Rust `pulp` front door,
its `pulp-cpp` sibling, and `pulp-mcp`. The live target was the real macOS
standalone process fixture with the `develop` profile and its own private
runtime directory.

Starting with only the public inspect guide and the project path, the client:

1. ran `pulp inspect list --json` through the installed Rust binary;
2. copied the exact session, instance, and publication IDs;
3. authenticated `pulp inspect capabilities --json` and explained the effective
   `session.control`, `state.read`, `state.write`, and `capture.image` authority;
4. read parameter 1 at `0 dB`;
5. sent typed `State.setParameter` with ID 1 and value `6.0`;
6. reread parameter 1 at `6 dB`;
7. saved a whole-window PNG response (50,296 base64 bytes); and
8. called installed `pulp-mcp`'s `pulp_inspect_capabilities` with the same exact
   identity and received `structuredContent.ok: true`.

The marketplace-style configuration was checked against `.claude-plugin/plugin.json`,
`.claude-plugin/marketplace.json`, `.mcp.json`, and the installed MCP binary.
The `/inspect` command now names installed commands only and documents the full
orientation/read/write/verify/evidence loop.

Result: pass. No source-build path or undocumented port/credential step was
needed. The only friction found was that the original guide omitted the named
orientation commands and exact evidence loop; the guide and CLI reference were
updated in this phase.
