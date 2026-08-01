---
name: inspect
description: Discover and verify an explicitly inspector-enabled standalone session
---

Launch a GPU-enabled desktop target explicitly with `pulp run --inspect` for
the `develop` profile, or `pulp run --inspect=observe` for read-only work.
Ordinary runs and plugin scanning remain off and publish no endpoint.

```bash
pulp inspect profiles --json
pulp inspect list --json
pulp inspect capabilities \
  --session SESSION_ID --instance INSTANCE_ID \
  --publication PUBLICATION_ID --json
pulp inspect doctor \
  --session SESSION_ID --instance INSTANCE_ID \
  --publication PUBLICATION_ID --json
```

Use this sequence for agent work:

1. Discover with `list` and copy the exact session, instance, and publication.
2. Read `capabilities`; explain what the selected profile permits.
3. Perform the requested typed read or `State.setParameter` mutation with all
   three selectors. Never mutate through `Runtime.evaluate`.
4. Reread the state or request `Capture.screenshot` with the same selectors.
5. Report the exact identity, request, response, and saved evidence path.

For the raw generic command, `State.setParameter` takes an integer parameter
ID and a numeric value:

```bash
pulp inspect --session SESSION_ID --instance INSTANCE_ID \
  --publication PUBLICATION_ID --command State.setParameter \
  --params '{"id":7,"value":0.75}'
```

Use the selected target's reported parameter ID and reread
`State.getParameters` with the same three selectors after the mutation.

The equivalent MCP flow is `pulp_inspect_list` →
`pulp_inspect_capabilities` → a typed `pulp_inspect_*` read/mutation →
`pulp_inspect_screenshot`. MCP uses the installed in-process client library;
it does not require a Pulp source checkout or a sibling CLI binary.

For an installed workflow, use `pulp` and `pulp-mcp` from the same Pulp CLI
installation. The Rust `pulp` front end resolves its sibling `pulp-cpp` even
when the current directory is a fresh project or scratch directory.

`Capture.screenshot` returns the selected standalone window's compositor PNG.
`Capture.screenshotNode` is still unavailable. `Runtime.evaluate` is denied by
ordinary profiles; a separately built and explicitly enabled custom host may
expose it, and that opt-in is remote code execution.

The CLI's `--output` option saves the screenshot response JSON, not a PNG.
Decode the base64 payload and verify its PNG signature before reporting it:

```bash
pulp inspect --session SESSION_ID --instance INSTANCE_ID \
  --publication PUBLICATION_ID --command Capture.screenshot \
  --output inspector-screenshot.json
python3 - <<'PY'
import base64, json, pathlib
response = json.loads(pathlib.Path("inspector-screenshot.json").read_text())
assert response["mimeType"] == "image/png"
png = base64.b64decode(response["data"], validate=True)
assert png.startswith(b"\x89PNG\r\n\x1a\n")
pathlib.Path("inspector-screenshot.png").write_bytes(png)
PY
file inspector-screenshot.png
```

For MCP, decode the same `data` field returned by `pulp_inspect_screenshot` and
apply the same signature check. Report both the `.png` path and exact selected
session identity.

The inspector exposes:
- View hierarchy with bounds, flex properties, and styles
- Widget state (values, labels, visibility)
- Theme tokens and computed colors
- Layout debug information

Auto-discovery reads owner-private ephemeral records and credentials, selects
an exact session/instance/publication generation, and authenticates both peers
with role-separated per-connection nonce/HMAC proofs bound to that generation.
Mutations additionally require the session's controller lease.
