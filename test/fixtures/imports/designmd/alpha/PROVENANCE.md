# Fixture Provenance

`DESIGN.md` in this directory is a verbatim, byte-identical copy of the
`paws-and-paths` example from Google's DESIGN.md format spec repository.

- **Upstream**: https://github.com/google-labs-code/design.md
- **Pinned tag**: `0.1.1` (commit `6589f05166473ddc54ca01a615254a673add492c`, published 2026-04-21)
- **Upstream path**: `examples/paws-and-paths/DESIGN.md`
- **License**: Apache-2.0 (see `/NOTICE.md` for the upstream attribution)

This pin is referenced by:

- `/NOTICE.md` (Apache-2.0 redistribution attribution)
- `/docs/reference/licensing.md` (public licensing page → propagates to generouscorp.com/pulp/licensing.html)
- `/compat.json` → `imports.designmd.detected-formats[].notes`

## Updating the pin

When upstream tags a new release:

1. Verify the new upstream fixture against this directory:
   ```bash
   gh api repos/google-labs-code/design.md/contents/examples/paws-and-paths/DESIGN.md?ref=<new-tag> \
     --jq .content | base64 -d | diff - DESIGN.md
   ```
2. If the diff is empty, just bump the tag string in the three files above.
3. If the diff is non-empty, decide: take the upstream changes (replace `DESIGN.md` byte-for-byte and bump the pin) OR keep the pin frozen at this tag and document why.
4. If upstream's `version:` frontmatter key changes value (currently `alpha`), add a new entry under `compat.json` → `imports.designmd.detected-formats` rather than mutating the existing one — old projects with the previous version stay parseable.

The `hand-authored.md` fixture and the three decoys under `decoys/` are Pulp-authored and not subject to the upstream pin.
