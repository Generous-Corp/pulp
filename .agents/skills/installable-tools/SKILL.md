---
name: installable-tools
description: The acceptance bar for anything Pulp can install (a `pulp tool` registry entry, `pulp add` package, or any downloadable). Validate the FULL lifecycle — install AND uninstall, from OUTSIDE a Pulp checkout, with the installed-user's binary — before its README or docs ship. TRIGGER when adding/editing tools/packages/tool-registry.json, a new binary_download/python_pip/npm_package tool, a `pulp add` importer, or writing docs that tell users to install something.
requires:
  scripts:
    - tools/packages/tool-registry.json
    - experimental/pulp-rs/src/tool_registry.rs
---

# installable-tools — validate before you promote

A thing Pulp can install is not "done" when the code compiles or when it works
in your repo checkout. It is done when a **user who installed Pulp** can install
it, use it, and uninstall it — from their own project directory, not the Pulp
source tree. Ship the README only after that is proven.

## Why this skill exists

`pulp tool install trace-processor` shipped as a first-class tool (with a whole
showcase README promoting it) but worked **only inside a Pulp git checkout**:
the registry is resolved by walking up from cwd for
`tools/packages/tool-registry.json`, which installed users don't have, so it
errored `Tool registry not found`. It got that far because every validation ran
from inside the repo. Uninstall had a second latent problem — it never validated
the tool id, so `uninstall ../../x` could delete outside the managed tree.

Both are the kind of gap that only a from-the-user's-seat, full-lifecycle test
catches. This skill is that checklist.

## The bar (all four, every time)

1. **Install from OUTSIDE any checkout.** `cd` to a scratch dir whose parents
   contain no `tools/packages/tool-registry.json`, point `PULP_HOME` at a throwaway
   dir, and run the install with the **user-facing `pulp`** (the installed binary
   or a fresh `./build/pulp`), not a raw in-repo build invocation.
2. **Use it.** Confirm the installed artifact actually runs / is found by the
   command that consumes it (`pulp trace query`, etc.).
3. **Uninstall from OUTSIDE any checkout.** It must remove exactly what it
   installed, **name the path it deleted**, and leave a clear message when
   nothing matched.
4. **Prove uninstall is safe.** A hostile id (`..`, `../x`, `a/b`, absolute
   path, empty) must be **refused before any deletion**, with a plant-a-victim
   test asserting nothing outside the managed tree was touched.

Only after 1–4 pass do you write or update the README/docs that tell users to
run the command.

## Registry resolution (why standalone works)

`pulp tool` resolves its registry as: repo `tools/packages/tool-registry.json`
(walk up from cwd) **first** — so a Pulp dev's edits show without a rebuild —
else the copy compiled into the CLI (`EMBEDDED_REGISTRY_JSON` /
`tool_registry::resolve_registry`). Adding a tool to the JSON is enough for the
Rust-native verbs (`list`/`info`/`path`/`doctor`/`uninstall`, and any tool whose
install is handled Rust-side) to work standalone. **Known boundary:** archive
tools whose install delegates to `pulp-cpp` (tar/zip extraction — uv, deno,
ffmpeg) still need the registry reachable by the C++ side; a bare-binary or
self-fetching tool (like trace-processor, which routes to its verified fetcher)
does not. If you add a delegated archive tool, validate its standalone install
explicitly or thread the registry to the delegate.

## Friendly aliases (discoverability)

A tool may list `aliases` in its registry entry so a natural name resolves to the
canonical id — `pulp tool install perfetto` reaches `trace-processor`. Resolution
happens once at the `run()` dispatch boundary (`ToolRegistry::canonical_id`), so
every verb (install/info/uninstall/path/doctor/update) accepts the alias. This is
what lets a Claude Code plugin user in a Pulp project say "install perfetto" and
have it work. Add an alias when the tool's product name differs from its id;
keep it exact-match (no fuzzy matching that could mis-resolve).

## Uninstall safety (deleting is sensitive)

`uninstall_tool` (`tool_registry.rs`) is the one place that calls
`remove_dir_all`. Two independent guards, both tested:

- `validate_tool_id` rejects any id that is not a single safe path component.
- After joining a validated id, the target must be a **direct child** of a
  managed root (`tools/<id>`, `tools/python-envs/<id>`, `tools/npm-packages/<id>`).

It returns the removed `PathBuf` so the command can tell the user exactly what
was deleted. Never delete silently; never widen the id contract without adding a
hostile-id test.

## The recipe

```bash
BIN=./build/pulp                     # or the installed ~/.pulp/bin/pulp
H=$(mktemp -d)                       # throwaway PULP_HOME — never the real one
cd "$(mktemp -d)"                    # scratch cwd, no registry above it

PULP_HOME="$H" "$BIN" tool info      <id>    # resolves? (embedded fallback)
PULP_HOME="$H" "$BIN" tool install   <id>    # installs from here?
PULP_HOME="$H" "$BIN" tool uninstall <id>    # removes + names the path?
PULP_HOME="$H" "$BIN" tool uninstall ../x    # REFUSED (exit 2)?
rm -rf "$H"
```

Unit-test equivalents live in `experimental/pulp-rs/src/tool_registry.rs`
(`resolve_registry_falls_back_to_embedded_outside_a_checkout`,
`uninstall_tool_rejects_hostile_ids_without_deleting`) and
`experimental/pulp-rs/src/cmd/tool.rs`.

## Docs that promote an installable

When a README/guide tells a user to install something, the exact command in it
must be the one that works for an **installed** user (validated per above). If
two commands do the same thing, show one — don't paste a commented-out alias
(it copies badly). Document how to **remove** it too, and warn that removal
deletes files.

## Related extend surfaces

`packages`, `kits`, `content`, and `installable-tools` are Pulp's four ways to
extend a project or machine, and they share one lifecycle contract: **add is
validated, remove is confirmed + confined to the surface's own area + names what
it deleted, and both add and remove ship tests.** Pick the right surface and read
the shared contract in
[extending-pulp.md](../../../docs/reference/extending-pulp.md).

- [`packages`](../packages/SKILL.md) — third-party audio DSP libraries → a project
- [`kits`](../kits/SKILL.md) — reusable Pulp code/UI/templates → a project
- [`content`](../content/SKILL.md) — data-only packs (presets/samples) → an installed plugin
- [`installable-tools`](../installable-tools/SKILL.md) — machine-level dev/agent tooling under `~/.pulp/tools/`, plus the shared validate-and-uninstall-from-outside-a-checkout bar
