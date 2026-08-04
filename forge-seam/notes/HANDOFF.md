# HANDOFF — Forge Modular, 30 Jul 2026

Branch `explore/modular-rack`, everything committed. The seam is synced
(`forge-seam/sync.sh --check` is clean), so nothing lives only in `/tmp`.

The goal is `GOAL-forge-modular-m5.md`. This says what is done, what is left,
and what will bite whoever picks it up.

---

## Where it stands

| | |
|---|---|
| Part 1 — it works everywhere | steps 1–6 done; **step 7 partly proven** |
| Part 2 — patches worth distributing | library built and gating; **12 of 12 prompts hold** |
| Part 3 — looks like the prototype | panel, degradation, spec table, legend, meta pill, glyphs, hover-from-stage and the Rack presence pill **done** |
| Part 4 — explanations teach | sidecars, structural roles, real port names, one-per-role primer, computed overview **done**; why-lint **done** |

### Evidenced

- **Step 5.** `auval -v aufx FrgR Gnrs` → SUCCEEDED. CLAP 33/44 — the two
  failures (`param-set-events`, `param-set-no-cookies`) are in the **shared
  Pulp adapter**; Forge FX fails the same kind, so they are not ours. VST3
  factory symbols present. All four signed inner-out, **notarized Accepted**,
  stapled, `spctl` → *Notarized Developer ID*.
- **Step 6.** M5 carries all four bundles; AU reads `FrgR` (was a stale
  `FgMd`); Gatekeeper accepts on a machine that did not build them; the
  toolchain proves itself there — 54 idioms, 114 negative controls, 0 wrong.
- **Step 7, partly.** The M5 carries the current build of all four bundles,
  Developer ID signed, **notarized Accepted and stapled**; the app assesses as
  *Notarized Developer ID* and the three plugins validate their staples.
  (`spctl --assess --type exec` on a plugin says "does not seem to be an app" —
  that is the wrong assessment type for a bundle, not a finding.)
  Locally: CLI PASS — a patch generated and held its idiom.

  The M5's CLI does NOT pass, and until today the instrument could not have
  told you either way: `prove_surfaces.sh` promised FORGE_HOST in its header
  and never read it, so naming a remote host ran the proof HERE and printed
  PASS. It re-execs over SSH now and refuses to fall back. The honest failure
  is the login keychain: over SSH the model CLI cannot reach its credential,
  so that surface needs a window on the M5 or an unlocked keychain.
- **The dozen prompts**: 12 held, 0 did not — run end to end, four of them
  reaching their idiom by implication rather than by name. Every failure on
  the way was the checker rejecting a correct patch, or the model being told
  nothing about what it got wrong; never patch quality. Kept at
  `tools/rack/patch_idioms/regressions/dozen-prompt-run.txt`, with every
  failed attempt's patch beside it.

---

## What the M5 carries

Everything, as of the 22:41 build — app and all three plugin formats, Developer
ID signed, notarized, stapled, assessing as *Notarized Developer ID*; the
generator toolchain; the module pack; and the port map (19 modules), without
which every VENDOR module draws with no jacks and its cables stop at the panel
edge.

Verified ON the M5 rather than inferred: `@ForgeModular/VCO` resolves to one
hit there, and a bare `VCO` finds ours first. Before tonight it resolved to
nothing — the app inserts a qualified slug and the resolver only understood
bare names, so every mention ever inserted was unusable by the thing that
consumes it.

Eight app fixes and six pinned joins are in it. Four of the eight were found by
rendering a frame and looking at it rather than by a test failing, and three
had shipped green while being invisible or unreachable.

## What is left

1. **The M5 CLI fails on the login keychain.** Diagnosed; it is not an empty
   error any more. The generator prints what it actually hit:

       the model CLI is not logged in for this session.
       Over SSH this is usually not a missing login but an unreachable one:
       the credential sits in the login keychain, and a non-interactive
       session may not open it.

   The harness reports the generator's own words now rather than a fragment
   of them; over SSH to the M5 it says:

       the model CLI is not logged in for this session.
       It said: Not logged in · Please run /login

   The credential IS there — `security find-generic-password -s "Claude
   Code-credentials"` finds it in `login.keychain-db` — so "not logged in" is
   the CLI's reading of a credential it cannot reach, not a missing one. This
   surface needs a window on the M5, or

       security unlock-keychain ~/Library/Keychains/login.keychain-db

   run there first. Reproduce:

       FORGE_HOST=m5 bash tools/rack/prove_surfaces.sh cli

   **That command only began meaning anything today.** prove_surfaces.sh
   promised FORGE_HOST in its header and never read the variable, so naming a
   remote host ran the proof on the machine you were sitting at and printed
   PASS. Any earlier "the M5 CLI passes" was a claim about the local machine.
   It re-execs over SSH now and refuses to fall back.

2. **REAPER on the M5, with a person watching.** Load AU, VST3 and CLAP, open
   each editor, generate from inside one. This is the part `prove_surfaces.sh`
   reports as SKIP rather than pretending to cover — a SKIP is not a PASS.

3. **The app on the M5, by pressing Build.** Needs a live GUI session there.
   `prove_surfaces.sh app` drives it, capped, and quits afterwards.

4. ~~**One of twelve prompts still fails**~~ Done, with a caveat worth
   keeping: twelve of twelve on a clean run, but the rate MOVES. Five runs
   went 6, 11, 10, 12, 10, 12 with the model unchanged. Every failure was the
   checker rejecting a correct patch, the generator being told nothing about
   what it got wrong, or once a crash in the harness — never patch quality.
   A correct patch can be wired several ways, so a too-narrow rule fails at
   random: treat a moving rate as a checker problem before a model one.
   `tools/rack/patch_idioms/regressions/dozen-prompt-run.txt` carries the
   history, and every failed attempt's patch sits beside it.

5. ~~**Our "Open in Rack" lets Rack restore its autosave.**~~ Done:
   `rack_open_command()` passes the patch positionally on a cold start and as a
   document to a running Rack, with tests for both. The autosave trap below
   still describes Rack's behaviour, which has not changed.

6. ~~**Two Part 3 items**~~ Done: hovering a cable in the rack lights its
   line in the explanation, driven from real pointer events in both
   directions, and `RackPresence` surfaces running / installed / plugin-only /
   absent in the pill rather than inferring it.

---

## Traps, all of them paid for

- **Rack will not quit from a script.** `osascript` to the app name and to the
  bundle id both do nothing, on either machine. It needs a click on its window.
  **Never hard-kill it**: that truncates the log and Rack returns with a
  crash-recovery modal which silently swallows the next patch argument.

- **The stray module is Rack's autosave.** `~/Library/Application
  Support/Rack2/autosave/patch.json` held one `TURBID` and zero cables for
  days, and Rack restores it on every launch. It is not our patch failing to
  load; it is Rack showing the previous session first.

- **Rack must RUN once before its plugins exist.** Installing the app is not
  enough — nothing is unpacked until first launch. The M5 went from 12 modules
  (Core only) to 73 after one 45-second run.

- **macOS ships no `timeout`.** It is GNU coreutils. Every capped command dies
  with "command not found", which reads as the surface failing rather than the
  harness missing a tool. `prove_surfaces.sh` shims it.

- **A non-interactive SSH session has a short PATH.** No Homebrew. `claude`
  runs plugin hooks with `node`, so the failure arrives as "node: command not
  found" from a hook nobody here wrote. `toolpaths.py` is the fix and the only
  place that answers "where are the tools".

- **rsync and the remote path.** It does not expand `~` inside a quoted
  destination, and the remote shell splits the spaces in `Application
  Support/Forge Modular`. Both report *"server receiver mode requires two
  argument"*, which names neither. macOS ships openrsync, so `--protect-args`
  does not exist — escape by hand.

- **An input takes ONE cable.** Rack keeps the last and drops the rest
  silently. Five idioms in our own library broke this before the fixture began
  enforcing it. It is now stated in the contract, with the remedy (sum through
  a mixer).

- **Do not `git add -A` here.** It stages the `planning` gitlink. There is an
  uncommitted `planning` pointer change in the tree right now — leave it.

---

## How to pick it up

```bash
cd /Volumes/Workshop/Code/pulp-modular-rack        # explore/modular-rack
forge-seam/sync.sh --check                          # nothing stranded in /tmp?
tools/rack/test_idioms.py                           # library: self-test + corpus
tools/rack/prove_surfaces.sh                        # step 7, locally
FORGE_HOST=m5 tools/rack/setup_m5.sh --check        # what the M5 has
```

The Forge-side shell is built in a throwaway worktree at `/tmp/forge-cur`; if
it is gone, the sources live in `forge-seam/modular/` and the chrome changes in
`forge-seam/patches/`. **Run `forge-seam/sync.sh` before finishing any session
that touches it** — macOS clears `/tmp`, and another agent may rebuild that
checkout.

The suite there is `forge-test-chrome-no-leak`: **583 assertions, 61 cases**,
including the no-leak baselines that prove nothing Rack-shaped reached Forge
Instrument, MIDI or FX.
