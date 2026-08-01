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
- **Step 7, partly.** On the M5: AU, VST3 and CLAP all installed with verifying
  signatures. Locally: CLI PASS — a patch generated and held its idiom. The
  M5's CLI does NOT pass; see item 1.
- **The dozen prompts**: 6 held, 6 did not. All 12 now RESOLVE to an idiom,
  where 2 previously matched nothing. Kept at
  `tools/rack/patch_idioms/regressions/dozen-prompt-run.txt`.

---

## What is left

1. **The M5 CLI still fails, and it is NOT the PATH bug.** That one is fixed:
   the re-run got past `node: command not found`. It now fails as

       model call failed:

   with **empty stderr** — the `claude` CLI exiting non-zero and saying
   nothing. Untouched and undiagnosed; do not assume it is the same problem
   wearing a new hat.

   Worth trying first, in this order: run `claude -p hello` over SSH on the M5
   by hand and see what it says; check whether it needs an interactive login or
   a credential this session does not have; compare `claude` version there
   against the machine where the same command works. Reproduce:

       ssh m5 'cd ~/Library/Application\ Support/Forge\ Modular/tools/rack && bash ./prove_surfaces.sh cli'

   Note the generator swallows the CLI's own stderr into that one line, which
   is why the message is empty. Fixing the reporting is probably the cheapest
   route to the cause.

2. **REAPER on the M5, with a person watching.** Load AU, VST3 and CLAP, open
   each editor, generate from inside one. This is the part `prove_surfaces.sh`
   reports as SKIP rather than pretending to cover — a SKIP is not a PASS.

3. **The app on the M5, by pressing Build.** Needs a live GUI session there.
   `prove_surfaces.sh app` drives it, capped, and quits afterwards.

4. ~~**One of twelve prompts still fails**~~ Done: all twelve hold. Every
   failure turned out to be the checker rejecting a correct patch, or the
   model being told nothing about what it got wrong — never patch quality.
   The run is `tools/rack/patch_idioms/regressions/dozen-prompt-run.txt`,
   counted from the file rather than remembered, with every failed attempt's
   patch kept beside it.

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
