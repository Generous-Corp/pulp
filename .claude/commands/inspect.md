---
name: inspect
description: Discover and verify an explicitly activated Development Inspector
---

Use the installed `pulp` command. It delegates this command to its installed
sibling `pulp-cpp`; do not rely on a source-tree `./build` path.

Start a GPU-enabled desktop standalone explicitly with `pulp run --inspect`
(develop) or `pulp run --inspect=observe`. Normal runs, plugin-format launches,
GPU-off/mobile builds, and incompatible external window hosts publish nothing.

Follow this evidence loop:

1. Run `pulp inspect doctor --json` and `pulp inspect profiles --json`.
2. Run `pulp inspect list --json`. Copy all three exact fields from one entry:
   `sessionId`, `instanceId`, and `publicationId`.
3. Run:

   ```bash
   pulp inspect capabilities --json \
     --session SESSION_ID \
     --instance INSTANCE_ID \
     --publication PUBLICATION_ID
   ```

   Explain the selected profile and effective capabilities. Never infer write
   authority from the profile name alone.
4. Read with the same exact selector, for example:

   ```bash
   pulp inspect --session SESSION_ID --instance INSTANCE_ID \
     --publication PUBLICATION_ID \
     --command State.getParameters
   ```

5. Only when `session.control` and `state.write` are effective, perform a typed
   `State.setParameter` request using an ID and legal value from the read. Never
   use `Runtime.evaluate` as a mutation shortcut.
6. Repeat the read, and optionally request `Capture.screenshot`. Save the JSON
   or screenshot response with `--output FILE` and report the exact three-part
   identity, requested method, before/after values, and artifact path.

Every live operation must keep the same exact identity. If the publication
disappears or changes, stop and rediscover; publication IDs are non-reusable.
Structured errors such as `capability_unavailable`, `selection_failed`, and
`mayHaveApplied` are evidence. Do not automatically retry a mutation reported
with `mayHaveApplied:true`.

When the selected standalone reports effective `test.input`, use the bounded
typed controls without evaluation:

```bash
pulp inspect inject-midi --kind note_on --channel 1 --note 60 --velocity 100 \
  --duration-ms 250 --json \
  --session SESSION_ID --instance INSTANCE_ID --publication PUBLICATION_ID
pulp inspect set-transport --playing true --position-samples 0 --tempo-bpm 120 --json \
  --session SESSION_ID --instance INSTANCE_ID --publication PUBLICATION_ID
```

The MCP equivalents are `pulp_inspect_inject_midi` and
`pulp_inspect_set_transport`. Only note-on/off and standalone
play/position/tempo are in `test.input`; parameters stay under `state.write`,
authoring controls stay under `authoring.tweaks`, and generic
preset/filesystem operations remain unavailable. Outstanding injected notes
are released on lease loss, disconnect, or teardown. A named `note_on` requires
`--duration-ms 1..2000`; its matching note-off is sent on the same connection
before that one-shot controller lease is released.

Installed `pulp-mcp` exposes the same flow as `pulp_inspect_profiles`,
`pulp_inspect_list`, `pulp_inspect_capabilities`, typed read/mutation tools, and
`pulp_inspect_doctor`. MCP success payloads include the exact session identity;
failures return `structuredContent.ok=false` with `code`, `message`, and `data`.

Whole-window capture is available only when the selected standalone host
provides compositor capture. `Capture.screenshotNode` remains unavailable.
Standalone profiles do not grant `runtime.eval`; a separately wired custom host
that enables it exposes remote code execution and must be treated accordingly.
