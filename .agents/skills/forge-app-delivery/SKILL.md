---
name: forge-app-delivery
description: Building and shipping a Forge app (Modular, Instrument, MIDI, FX, and the sequencer work to come) as a signed installer somebody else can actually use. Covers the seam between the Forge and Pulp repos, what a green signal does and does not prove, shipping the runtime rather than only the binary, and the wiring gaps that make a finished feature behave like a missing one.
---

# Delivering a Forge app

A Forge app is not one binary. It is a shell compiled in a *different repository*,
a Python generator, a Rack plug-in, three plug-in formats, an uninstaller and a
toolchain the user's machine fetches for itself. Every delivery failure recorded
here came from one of those parts being absent, stale, or present but unreachable
while every check reported success.

Read this before packaging anything, and before concluding that a feature "works".

## The rule that would have saved the most time

**Verify identity, not size, and never a signature.** In a single day this
project shipped:

- a **292-byte** package that signed, notarized, stapled and passed Gatekeeper
  while containing nothing (staged with symlinks instead of `ditto` copies)
- three plug-in bundles of **72 KB** that installed cleanly and contained no
  code (CMake creates `Contents/MacOS` at *configure* time, so `-e` is true for
  an empty husk)
- a **76 MB, four-payload, correctly-sized, notarized** installer containing
  **the wrong application entirely**

The third is the important one. Size caught the first two and was useless for the
third, because the payloads were the right size. Only identity distinguishes a
correct build from a plausible one. Pick a string that exists in the current
build and cannot exist in a stale or wrong one, and assert it in the packaging
script:

```bash
hits=$(strings "$binary" 2>/dev/null | grep -cF "$SHELL_MARKER" || true)
```

Then expand the finished package and check the payload, rather than trusting the
script that made it:

```bash
pkgutil --expand "Foo.pkg" out      # --payload-files does NOT recurse into
                                    # nested component payloads
```

## The seam: the shell is built in the other repo

Forge app shells live in `forge-seam/`, are copied into a **throwaway Forge
worktree** by `forge-seam/populate.sh` (`/tmp/forge-cur`), compiled there, and
copied back by `forge-seam/sync.sh`.

Consequences that have each cost a cycle:

- **`/tmp` is cleared.** Run `forge-seam/sync.sh` before finishing any session
  that touched a shell source, or the work is gone.
- **Changes to Forge-repo files must travel in `forge-seam/patches/`** or they
  are lost on the next repopulate.
- **Clangd diagnostics on `forge-seam/*.cpp` are noise.** Those files only
  resolve their includes inside the Forge worktree. A wall of "no type named
  `string` in namespace `std`" means you are reading the file outside its build,
  not that the file is broken.
- **There may be two apps of the same name.** Forge Modular exists both as the
  real shell (Forge worktree) and an older `examples/<app>/app/src/shell.cpp`.
  A packaging script that hardcodes one path cannot be pointed at the other by
  any argument. **Print which artifact you selected** — a silent choice between
  two same-named apps is how the wrong one shipped.

## Ship the runtime, not just the binary

A Forge app runs a Python generator. If the installer carries the app and not the
generator, it installs and does nothing.

This shipped. The package had the app, three plug-in formats and the Rack modules,
and **zero Python**. It went unnoticed because the build machine had the tools
seeded into `~/Library/Application Support/<app>/tools/` by an earlier manual
step, and that path wins the lookup.

Two halves are required and either alone still ships a dead app:

1. Package staging copies the tools into the bundle **before signing**, so the
   signature covers them.
2. The app's `tools_dir()` looks **inside its own bundle** (walk up from
   `_NSGetExecutablePath` to `Contents`, then `Resources/tools/…`), after the
   Application Support copy so a user-replaceable copy still wins.

**Test from a clean state.** Move the seeded directory aside before testing, or
you are exercising the fallback that hides the bug:

```bash
mv ~/Library/Application\ Support/<app>/tools{,.bak}
```

## Present but unreachable is the most expensive defect class

Every one of these was a *finished* feature that behaved exactly like a missing
one, and each was found by a user rather than a test:

| Symptom | Cause |
|---|---|
| Auto-download never worked | token sent as a query parameter; the API wants a cookie, so it had returned 403 for every plugin since it was written |
| Patches used self-built lookalikes of famous free modules | the model was never given an inventory, so it could not know they existed |
| `@`-mention told the user to go install it themselves | the download function existed and that path did not call it |
| A setting had no effect | it was added to the defaults and read by nothing |
| A setting could not be changed | it existed only in a JSON file with no UI |
| A shipped fetch script did nothing | nothing invoked it, and it installed to a third path neither reader used |

Before claiming a capability works, trace it end to end from the surface the user
touches. "The function exists" is not the claim being made.

Two design rules fell out of this:

- **One resolver per resource.** Three components disagreeing about where the
  Rack SDK lived meant fetching it correctly still failed. If you add a second
  fetcher, look for the first one — it usually exists.
- **Cost is friction, not price.** Ranking modules by `premium` put everything
  free ahead of the 70 premium plugins the user had *bought*. Sort by what it
  takes to obtain a thing (installed → owned → free → unavailable), and never
  conflate "premium" with "not owned". Downloading something already paid for is
  not a purchase.
- **Measure entitlement, never infer a tier.** The library API exposes what an
  account owns and not its subscription level. Ownership is the better signal
  anyway: it is correct for a user on any plan who bought modules individually.

## Signals that have lied

- **Exit code 0 from a backgrounded launcher** means the launcher exited, not
  that the work succeeded. Wait on the process:
  `until ! pgrep -f "<cmd>"; do sleep 10; done`
- **`grep -qF` under `set -o pipefail`** exits on first match, SIGPIPEs the
  upstream command, and fails the pipeline — so a binary that *does* contain the
  marker is rejected *for* containing it. Count instead; it drains the stream.
- **`find … | head -1`** in `Contents/MacOS` returns `libwgpu_native.dylib`,
  which is copied in beside the executable and sorts first. Prefer the file named
  after the bundle.
- **zsh aborts the whole command** when any glob matches nothing, so
  `ls a/*.x b/*.x` reports nothing found even when `a/` has matches. That
  produced a false "the Rack modules are missing" conclusion.
- **A test satisfied by nothing happening.** A cache test asserting "fewer than
  16 builds" passed while measuring zero activity. Assert the work happened
  (`REQUIRE(cache.stats().builds > 0)`) before asserting it was cheap.
- **`sample` and other profilers fail silently over SSH** (TCC is per-process).

**Every test ships broken-on-purpose once.** A test that has never failed has not
been tested. State the mutation and its result when reporting.

## Installer text

- **Do not hard-wrap the licence/Read Me text.** macOS rewraps it to the pane
  width and pre-wrapped lines come out ragged. One long line per paragraph, blank
  lines between, indented list items preserved.
- **Quote every path you print.** App names contain spaces, so
  `/Applications/Forge Modular.app/...` unquoted is read as far as `Forge` and
  reports "no such file or directory" — which reads as a missing uninstaller
  rather than a quoting mistake. Show the quoted form and say why.
- **Say where the uninstaller is.** "An uninstaller ships inside the app" without
  a path means nobody runs it.
- **Do not promise an interaction you did not build.** The pane said the app
  "will offer to download… when you say yes"; what exists is announce-and-fetch
  governed by a setting. Either build the prompt or soften the sentence.
- **Mark the payload others depend on `required`** (`enabled="false"
  selected="true"`). For Forge Modular that is the app, because the Rack modules
  and the uninstaller live inside its bundle.

## Licence boundaries that must not drift

- The **Rack SDK is GPLv3 and VCV's**. It is never in a shipped artifact. The
  user's machine fetches it, which is not redistribution.
- A **module built against that SDK inherits GPLv3**. Fine for personal use; the
  licence follows if it is distributed.
- The **`.vcvplugin` is the only artifact linking the SDK.** Keeping it a
  separate payload from the app preserves that boundary; merging them blurs it.

## Notes for future Forge apps

Sequencer-specific notes go here when that build starts. The parts above are
app-agnostic: the seam, the staging, the identity check and the wiring-gap class
apply to any Forge shell. What tends to differ per app is the generator's
external toolchain (Forge Modular needs the Rack SDK and a C++ compiler) and what
"the runtime" means for it — enumerate that first, because it is the thing most
likely to be left out of the installer.
