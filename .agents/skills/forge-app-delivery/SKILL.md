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

Signed installer staging also has a hard precondition: Pulp's unattended
signing doctor must pass before the first production `codesign`. A missing or
failed preflight terminates packaging; never skip it or turn it into a warning,
because login-keychain fallback can open a GUI password prompt and wedge an
agent/SSH/CI session. The shared doctor owns dedicated-keychain repair, full
partition authorization, identity-hash selection, and the real timestamped
probe.

**Test from a clean state.** Move the seeded directory aside before testing, or
you are exercising the fallback that hides the bug:

```bash
mv ~/Library/Application\ Support/<app>/tools{,.bak}
```

## An installer must be able to update what it installs

The app's working copy of the generator lives in Application Support and the
shipped one lives in the bundle. Preferring Application Support
*unconditionally* means a toolchain written by an older release shadows every
fix a newer one ships — and it fails **silently**, because the shadowed script
is old enough to reject a subcommand the new app calls.

That is exactly what happened: `library_catalog.py index` did not exist in the
August 1st copy, so the app asked for a library index on every launch, the
script printed its usage, exited 2, and four days passed with a 200-plugin
index nobody could explain.

- **Compare a version stamp written at package time, never an mtime.** Every
  path here is a copy and a copy rewrites mtimes. `package.sh` writes
  `Contents/Resources/tools/rack/VERSION` (version, then packaged-at);
  `choose_toolchain()` prefers the bundle only when it is **strictly** newer, so
  an equal stamp leaves the installed copy in charge and hand-editing it still
  works for development. An **unstamped** directory is the oldest thing there
  is, which is what every pre-stamp machine looks like.
- **`install_toolchain.sh` must not strip that stamp.** It `rsync --delete`s
  from its source, and a source *checkout* has no `VERSION` — so a developer's
  install would make the destination look older than the release and lose to it
  forever. Exclude `VERSION` when the source lacks one.
- **Anything spawned in the background must record its exit status**, somewhere
  the app reads. `library_index_command()` writes `runs/library-status`; the
  settings row turns that into "the refresh failed (exit 2), see …". A log
  nobody opens is the same as no report at all.

## The app must be able to say what it is

`package.sh --version` named the .pkg and nothing else, so an installed 0.12.7
answered `CFBundleShortVersionString` **0.11.0** and 12.6 was indistinguishable
from 12.7 on the machine. Stamp the version into the app **and all three
plug-in bundles** (staged copies, before signing, so the signature covers it),
then **read it back out of the expanded package** and refuse the release when
it disagrees.

Ship a details surface with it. The field that matters most is the **live
toolchain path** — had it been visible, a day of shadowed fixes would have been
obvious in seconds. Version, packaged date, that path and its stamp, index
count and age, Rack SDK location, and whether a VCV sign-in was found — never
the token. A `Label` cannot be selected with a mouse, so a Copy button is the
affordance; promising selectable text you do not have is the same kind of claim
as an installer promising modules it lacks.

## The settings pane does not clip, it collapses

Forge's settings card is a fixed 660 tall and the Permissions pane was not a
scroll view. A pane taller than the card therefore did not clip — flex shrank
whatever it could, captions collapsed to zero height, and every row drew on top
of the one below it. Adding one product row made it unmistakable; it was
already true of the built-in ones.

There is a second, independent cause worth knowing anywhere in Pulp: **Yoga's
measure callback asks a `Label` for its INTRINSIC width first**, and a
paragraph's intrinsic width is the whole thing on one line — so the height it
measures is one line however narrow the label is bounded to. Reserve the real
height explicitly with `label.measured_height(bound)` when you bound a
multi-line label's width.

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

The most recent entry is the subtlest, because every part of it behaved as
specified: **naming something did not fetch it.** A prompt that named a maker
expanded correctly into the model's brief, the model reached for that maker four
times, was told each time that the plugin was not installed, substituted
something else and said so honestly, and the count at the end read "0 module(s)
drawn from this maker". The download machinery worked and had one trigger — a
missing-CAPABILITY gap in preflight. A mention was not a trigger at all. **If a
surface lets somebody name a thing, naming it has to guarantee it is there before
the thing that consumes the name runs.** Bound what a *category* fetches (a maker
is a preference, so rank by the request and cap it) and keep the exactness for
what was named outright.

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

## The bundle is read-only, and a copy of it inherits that

The generator's working copy is laid down by `install_toolchain.sh` from
`Contents/Resources`, and an installed bundle is root-owned and sealed. Two
consequences, both of which stopped the first build on a genuinely clean
machine dead:

- **`rsync -a` reproduces the source's modes.** The module pack arrived
  `r--r--r--` and the panel emitter died on `PermissionError: .../res/ATT.svg`
  — after installing everything and verifying nothing. A copy that is going to
  be *rewritten* must be made writable explicitly.
- **macOS's `rsync` is openrsync, which ACCEPTS `--chmod` and ignores it.** No
  error, no warning, not one mode bit changed. Set the modes afterwards with
  `chmod -R u+rwX`, by a tool that does what it says.
- A failed first install leaves an unwritable tree that every later attempt
  also fails on, so reclaim the destination (`chmod -R u+rwX "$DEST"`) before
  copying.

Simulate this by making the staged bundle read-only (`chmod -R a-w`) before
testing the install path. A writable copy of the app tests nothing.

## A test program in the module pack breaks the behavioural gate

`examples/forge-modular/src/` holds the modules AND `test_portmap_merge.cpp`,
which has its own `main`. It compiled into the plugin dylib harmlessly for
months. The behavioural gate links those same objects beside *its* `main`, so
every module build ended in `duplicate symbol '_main'` — three attempts, three
model calls, several minutes — and the gate had therefore never passed for any
generated module.

Two rules fall out of it:

- Anything that links the pack's objects must exclude the standalone programs.
  `generate.py`'s `sources()` and the CMake glob both skip `_*` and `test_*`.
- **Print the tail of a link failure, not the lines containing `error:`.** The
  only `error:` line a linker emits is "linker command failed", which says
  nothing; the symbol is in the lines above it. The message read as a blank
  refusal for exactly as long as that filter existed.

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
- **A CRASHED SUBPROCESS READ AS A VERDICT.** The audibility gate segfaulted
  loading third-party Rack plugins. `returncode != 0` was taken to mean "this
  patch makes no sound", so six generations in a row ended "gave up after 3
  attempts" with an empty explanation and nothing anywhere saying a process had
  died. A negative return code is a signal, not an answer: check
  `returncode < 0` separately and say which signal and what it was loading.
  The same shape applies to any gate that shells out.
  **Naming it is only half.** The retry context still said "structurally valid
  but SILENT when run" whatever had happened, so the model was sent to fix a
  fault nobody had measured and the patch was discarded at the end anyway. A
  check that could not run must not feed the verdict path at all: keep the
  artifact, say the doubt out loud.
- **A HARNESS THAT STANDS IN FOR A FRAMEWORK MUST DO WHAT THE FRAMEWORK DOES,
  IN ITS ORDER.** Both of the gate's crashes were that, and both were found from
  a real backtrace (`~/Library/Logs/DiagnosticReports/*.ips`, or lldb with
  `settings set target.env-vars DYLD_LIBRARY_PATH=…` — the env var is stripped
  from a debugged process, so a run under lldb otherwise dies in dyld and looks
  like a different bug):
  - `EXC_BAD_ACCESS at 0x10` in a module's *constructor*. `APP` is
    `rack::contextGet()` and is null until something calls `contextSet()`;
    `Context::engine` sits at offset 0x10, so any module reading the sample rate
    while being built dies. Bogaudio's base module does it for all 111 models.
  - `EXC_BAD_ACCESS at 0x0` in a module's `process()`. **A constructed module is
    not a running one.** Rack sends `onSampleRateChange` then `onAdd` before it
    ever calls `process()`, and modules allocate their DSP buffers there — CV
    funk's Alloy sizes a delay line in it, so the harness read through a null
    pointer with a zero ring mask.
  Fixing the first uncovered the second, and each survives the other's fix, so
  one plugin is not a proxy for the other in a regression test.
- **The staging output directory may be left read-only.** Testing the install
  path with `chmod -R a-w` (above) leaves `--out` unwritable, and the next
  `package.sh` run dies in a wall of `rm: Permission denied` that reads like a
  packaging bug. `chmod -R u+rwX "$OUT_DIR"` before removing it.

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
  — `fetch_sdk.ensure()` says what it is about to do and then does it, and
  nobody is asked. The sentence now describes that. **Softening copy is only
  half the fix**, because it is correct only while the code stays
  announce-and-fetch: `check_installer_promises()` in `tools/rack/test_patch.py`
  asserts BOTH halves, driving `ensure()` with stubs to prove the order is
  announce-then-fetch and then scanning the pane for consent wording. Build the
  prompt later and that check is what says the pane has to be rewritten with it.
  Note the setting that governs it, `auto_fetch_sdk`, has **no control in
  Settings** (`settings_choices()` exposes module source, downloads, the time
  limit, the index refresh and the about pane, and not this one) — so do not
  write copy telling anybody they can switch it off there.
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

## A Forge shell is a bigger view tree than a plugin editor

Two per-frame walks in Pulp were written for a plugin editor's tens of views and
each asked libc++abi a question per node: `needs_continuous_frames` tried six
`dynamic_cast`s (three of them through multiple inheritance, so each miss walked
`__vmi_class_type_info`), and the host-parameter pump asked "is this a
DesignFrameView". Sampled on an idle Forge Modular window on an M3 Ultra those
two were **~29% of the process** — more than the Skia drawing and the Yoga layout
they were gating. Both are now a virtual call and a bool.

The general rule: **anything that runs over the whole view tree every frame must
not use RTTI**. A shell's tree is thousands of views, and 120 Hz multiplies
whatever a node costs by four or five orders of magnitude.

Measuring it: no `PULP_TRACING` in these builds, so `sample <pid> 10 -f out.txt`.
It fails silently over SSH (TCC is per-process), so run it from a window on the
machine. Read it by attributing each libc++abi run to its nearest non-libc++abi
ancestor, or the cost hides inside `dyn_cast_slow` where no Pulp symbol appears.
Thread count is not a smell by itself: an idle window here was 20 threads, all
accounted — main, CVDisplayLink, NSEventThread, four CoreAudio (`caulk*` +
`IOThread.client`, because the standalone opens a device), eight Dawn
`AsyncWorkerThreadPool` workers, one `BackgroundJobService`, four libdispatch
workers serving Metal/CoreAnimation queues.

## Notes for future Forge apps

Sequencer-specific notes go here when that build starts. The parts above are
app-agnostic: the seam, the staging, the identity check and the wiring-gap class
apply to any Forge shell. What tends to differ per app is the generator's
external toolchain (Forge Modular needs the Rack SDK and a C++ compiler) and what
"the runtime" means for it — enumerate that first, because it is the thing most
likely to be left out of the installer.
