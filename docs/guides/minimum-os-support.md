# Minimum OS Support

When you ship a plugin or app, users ask a simple question: **"Will it run on my
machine?"** The answer is the *minimum OS* — the oldest operating system your
build still runs on. This guide explains how Pulp figures that number out for
you, and the two commands you can use to check it yourself.

## The one idea

Your build is made of many pieces: Pulp's own code, the libraries Pulp bundles
(Skia and Dawn for graphics, optionally a JavaScript engine), the C++ runtime,
and the code *you* write. Each piece was built to run on some minimum OS.

> **Your minimum OS is the highest minimum among all those pieces.**

If Skia needs macOS 13.3 and everything else needs 11.0, your plugin needs
**13.3** — the highest one wins. Add one library that needs macOS 14 and your
whole plugin now needs macOS 14, whether you meant to or not. That's the trap
this tooling exists to catch.

The same idea applies per platform, just with a different unit:

| Platform | The "minimum OS" number is… |
|----------|------------------------------|
| macOS    | the deployment target (e.g. `13.3`) |
| Linux    | the oldest glibc it needs (e.g. `2.34`, which is roughly RHEL/Rocky 9) |
| Windows  | the Windows version baked into the executable (e.g. `10.0`) |

## Pulp pins the floor for you

You don't normally set this by hand. When you build against the Pulp SDK, Pulp
reads its own floor from one file — `tools/deps/min_os.json` — and pins your
build to it automatically (via `PulpMinOs.cmake`). One place to change, and it
flows to every plugin built against the SDK. If you *want* a higher minimum (say
you rely on a macOS 14 API), you can still raise it; Pulp only stops you from
setting it *below* what the bundled libraries actually support, because that
wouldn't run anyway.

So most of the time the floor is handled. The two commands below are for the
moments you want to **verify** it — during SDK upgrades, before a release, or
when a new dependency might have quietly raised the bar.

## Command 1 — check one binary

> "What minimum OS does *this* file actually need?"

```bash
pulp minos measure <path-to-binary>
```

It reads the answer straight out of the compiled file — no guessing, no manifest
to keep in sync. Point it at almost anything:

```bash
# a plugin bundle's inner binary
pulp minos measure "MyPlugin.vst3/Contents/MacOS/MyPlugin"
# → macho 13.3     (needs macOS 13.3)

# a shared library
pulp minos measure build/libmydsp.dylib
# → macho 13.3

# a Linux plugin
pulp minos measure MyPlugin.clap
# → elf 2.34        (needs glibc 2.34)
```

The output is `<kind> <floor>`, where `kind` is the binary format
(`macho` = macOS, `elf` = Linux, `pe` = Windows) and `floor` is the minimum OS.

This is the honest number because the compiled binary already carries the
fingerprints of everything that went into it. If a dependency dragged your floor
up, this is where you'll see it.

## Command 2 — check every project at once

> "Does the new SDK still build all our demos, and what OS does each one need?"

When you cut a new SDK, you want to know two things across *all* your downstream
projects: (1) does everything still build, and (2) did the minimum OS move. The
sweep answers both in one pass:

```bash
# see the plan first — no clones, no builds
pulp minos sweep --sdk-prefix /path/to/installed-sdk --dry-run

# run it
pulp minos sweep --sdk-prefix /path/to/installed-sdk
```

For each project it clones the repo, builds it against your installed SDK,
measures every artifact it produced, and prints a table:

```
SDK floor (this host): 13.3

repo                             build  floor    vs SDK   notes
---------------------------------------------------------------
pulp-example-plugins             ok     13.3     match
pulp-gpu-nam                     ok     13.3     match
```

- **build** — did it compile against the new SDK?
- **floor** — the minimum OS the built project needs.
- **vs SDK** — `match` if it equals the SDK's own floor; **`DRIFT`** if a project
  ended up needing a *higher* OS than the SDK declares. Drift almost always means
  a build machine leaked a newer toolchain than the SDK's prebuilts use — a real
  portability bug worth chasing.

The command exits non-zero if any project fails to build or drifts, so you can
wire it straight into a release gate.

Handy options:

```bash
--only pulp-example-plugins,pulp-gpu-nam   # just these repos
--json /tmp/sweep.json                     # machine-readable report
--dry-run                                  # print the plan and stop
```

### What it sweeps

The list of projects lives in `planning/sdk-consumers/consumers.yaml`. Projects
that are just a README or a downloadable package (no buildable source) are
skipped automatically. A few projects need an extra build flag (for example
GPU NAM builds with `-DGPU_NAM_USE_INSTALLED_PULP=ON`); those knobs live in
`tools/scripts/sdk_consumer_sweep_recipes.yaml` so the sweep stays turnkey.

The sweep needs an *installed* SDK to build against — the directory that contains
`lib/cmake/Pulp`. Make one from a Pulp checkout with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix /path/to/installed-sdk
```

It also needs PyYAML: `python3 -m pip install pyyaml`.

## Where to run each

Both commands are part of the Pulp CLI (`pulp minos ...`) and the `/minos` Claude
slash command. `pulp minos measure` is also available to agents over MCP as the
`pulp_minos` tool. The sweep is CLI-only because it clones and builds many
repositories — not something to fire off inside an editor session.

## Under the hood

- `pulp minos measure` → `tools/scripts/measure_min_os.py --measure`
- `pulp minos sweep`   → `tools/scripts/sdk_consumer_sweep.py`
- The SDK's own floor is declared in `tools/deps/min_os.json` and pinned into
  every build by `tools/cmake/PulpMinOs.cmake`.

For the release-side view of validating downstream projects against a new SDK,
see [Downstream Validation](downstream-validation.md).
