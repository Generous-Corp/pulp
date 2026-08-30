---
name: daw-smoke
description: Real-DAW (REAPER) functional smoke for reload/editor/format-adapter changes — opt-in, scoped, headless-safe, zero-pollution
---

# daw-smoke — functional reload/editor verification in a real DAW (REAPER)

Use this when you need proof that a **reload / editor / format-adapter** behavior
actually works **inside a host**, beyond what `auval` / `pluginval` /
`clap-validator` give (those prove scan+load, not functional behavior).

Harness: `tools/testing/daw-smoke/reaper_smoke.py` (+ `insert_and_float.lua` and
`sequence_loop_seek.lua`). The scraper + mode dispatch are unit-tested with no
REAPER in `tools/testing/daw-smoke/test_reaper_smoke.py`.
Full rules: `docs/guides/daw-smoke.md`. CLAUDE.md has the one-paragraph policy.

## Modes (`--mode`, default `reload`)
- **`reload`** — the original flow. Seed watched DSP variant A, insert+float the
  FX, copy variant B over the watched path, scrape `swapped DSP` / `reload
  rejected`.
- **`live-plugin-swap`** — a live plugin-INSTANCE swap inside a Pulp host that
  hosts another plugin in a `SignalGraph`. Insert+float the host FX, write a swap
  request to `--watched-swap-request`, then scrape `[live-swap] committed` (PASS)
  vs `live plugin swap refused` (FAIL). The success marker is NOT logged by the
  core swap path — `SignalGraph::prepare_swap` logs ONLY refusals. The host plugin
  must emit `[live-swap] committed` itself from its
  `NodeLiveSwapPolicy::on_instance_swapped` observer (that is the seam the smoke
  scrapes and the headless test exercises). The DAW-free, CI-runnable mirror is
  `test/test_signal_graph_live_swap_continuity.cpp`, which drives the same
  stage + `prepare_swap` commit through `process()` and asserts sample continuity
  (no dropout/xrun) across the swap block for every hosted format (VST3/AU/CLAP/
  LV2). Use the REAPER mode as the local-only in-host confirmation.
- **`sequence-loop-seek`** — set a loop region on the REAPER timeline, play, and
  perform scripted seeks (into/out of the loop and across the wrap) against a
  plugin that embeds a sequence. `sequence_loop_seek.lua` drives the transport
  via a deferred pump (`GetSet_LoopTimeRange` + `GetSetRepeat` + `OnPlayButton` +
  `SetEditCurPos` seeks), handshaking `FX_SHOWN`→`SEEKS_DONE` through the status
  file. The plugin emits per-block markers `[seq-loop] blk host_qn=.. seq_qn=..
  active=.. jump=.. dropout=..`; the scraper (`analyze_seq_loop_log`) asserts the
  sequence read position TRACKED the host playhead within `--pos-tolerance-qn` on
  every block — a free-running counter that ignores the host jump is caught as
  drift → FAIL. PASS requires >=1 wrap AND >=1 seek, note activity, and no
  dropout; otherwise INCONCLUSIVE (a SKIP/INCONCLUSIVE is never a PASS). This is
  the **harness half of Phase-2 DoD Proof #2** — the full proof also needs the
  embedded-sequence plugin (Gate-5/6) and a real REAPER run. The scraper is
  unit-tested with synthetic logs (incl. a negative drift/dropout case), so the
  parse logic is proven without REAPER.
- **`editor-open`** — insert the plugin, open its editor, confirm it rendered. For a
  product whose editor *is* the product, neither a hot-swap nor a transport scenario
  applies; the only question is whether it loads in a real host and draws. This is
  precisely the check the format validators cannot make: `auval` and `clap-validator`
  prove a plugin scans and instantiates, never that its window comes up. After
  `FX_SHOWN` it waits before tearing down, because an editor that opens and *then*
  throws reports itself in the seconds after the window appears.
- **`editor-build`** — generate a patch from *inside* the host. `editor-open` proves
  the window comes up; it does not prove the product behind it works. The generator is
  spawned by the plugin, and an editor that draws perfectly can still fail to reach it
  — the standalone once did exactly that, because an app launched from Finder inherits
  no PATH. Nothing short of triggering a real build inside the host tests that path.
  The trigger is a **file** the shell reads (`FORGE_MODULAR_TEST_PROMPT`), *not*
  synthetic clicks — see the gotcha below. The verdict is the generator's own success
  line plus the file it names, scraped from the plugin's log; never "the click
  appeared to land."
- All modes share the same REAPER lifecycle (`ReaperSession`): fresh portable
  dir, temp scan path, pre-warm scan, scripted insert+float, guaranteed teardown.
  They differ only in what they SEED, the TRIGGER they fire once the FX is shown,
  and how they VERIFY the captured log. Add a mode by writing seed/trigger/verify
  around `ReaperSession`, not by duplicating the launch plumbing.

## When to reach for it
- A headless / standalone / `render_to_png` capture PASSES but you're not sure it
  works in a host. It usually doesn't tell the whole story — see the gotcha below.
- Reload/editor/format-adapter change, especially **before asking a human to test**.
- Opt-in (default OFF), enabled per-machine in `~/.config/pulp/daw-smoke.toml`; a
  ship gate only when `enabled` + `gate` + the diff hits the allowlist. NOT every
  build. `DAW-Smoke: skip reason="..."` trailer bypasses a single commit.

## Gotchas (the expensive lessons)

### Copy a bundle with `ditto`, never `shutil.copytree`

A macOS code signature lives partly in extended attributes, and `copytree` does
not carry them. The copy the harness handed REAPER verified as **"code object is
not signed at all"** while the original was clean under `--deep --strict`. Use
`ditto`, and verify the COPY rather than the source:

```
ditto "$SRC" "$DST" && codesign --verify --deep --strict "$DST"
```

(The failure named a subcomponent — a non-executable `.json` in
`Contents/MacOS/`, where bundle convention expects only executables. Worth
knowing when reading such an error; `--deep --strict` treats everything in
`MacOS/` as a subcomponent.)

### A custom scan path does not publish on REAPER 7.78 — the editor modes cannot prove anything there

`editor-open` (and any mode needing the FX inserted) currently reports
INCONCLUSIVE on REAPER 7.78 with:

```
pre-warm scan did not publish the target into REAPER's plugin cache before timeout
REAPER did not open the FX within timeout (flaky launch) — INCONCLUSIVE
```

Reproduced four times at 90s, 150s and 240s timeouts, before AND after the
`ditto` fix, and with the plugin also present at the standard VST3 path — the
harness redirects REAPER to its temp `scan_dir` regardless, so installing
normally does not change the outcome. The user's own REAPER cache DOES contain
these plugins, so REAPER scans them fine through its ordinary paths; it is the
harness's `vstpath_arm64=<temp>` redirection that is not taking effect on 7.78.

**So a SKIP or INCONCLUSIVE here is not a PASS, and right now it is the
expected result rather than evidence about the plugin.** Note these modes also
exit **0** while reporting INCONCLUSIVE, so anything reading exit codes scores
them as passes. Until the scan-path redirection is fixed, prove editors another
way and say plainly that this rung is unproven.


- **Headless capture hides adapter-only bugs.** The format adapter injects state no
  headless path has. 2026-07-04: it *synthesizes a Bypass parameter* in a host, so
  the reload param-contract gate rejected every in-DAW reload (`parameter contract
  differs`) while every headless capture passed. Only REAPER surfaced it (fixed
  `4a6e048f4`: the contract gate now excludes an adapter-synthesized bypass). If you
  change reload/editor/adapter code, a real-DAW smoke is the check that catches this.
- **`screencapture` returns BLACK frames** in an agent/SSH context (no Screen-
  Recording TCC grant). Do NOT verify visually via screencapture. Verify by scraping
  the plugin reload log from REAPER's captured stdout (`swapped DSP` = applied,
  `reload rejected` + `reject-diff` = fail, `loaded initial logic` = it loaded). Use
  `render_to_png` (e.g. examples/hot-reload-morph capture) for human-viewable visuals.
- **REAPER launch is flaky** (scan dialogs, stuck instances, first-run per portable
  dir). Mitigate: fresh temp portable dir per run, a pre-warm scan launch you kill,
  timeouts, and an INCONCLUSIVE outcome — never a spurious FAIL. A `STATUS: None`/
  no-FX-load run is INCONCLUSIVE, not PASS.
- **Only ever kill the REAPER you started.** `pkill -9 -x REAPER` to clear
  "stragglers" kills every REAPER on the machine — including the one the person at
  the keyboard is working in, with their unsaved project. It was reported as "REAPER
  keeps restarting"; it was being killed and relaunched underneath them. Keep the
  owned child as a `Popen` object and stop it through `poll` / `terminate` / bounded
  `wait` / `kill` / `wait`; never send a later raw signal to its saved numeric PID,
  which may have been recycled after the child exited. `PULP_DAW_SMOKE_KILL_ALL=1`
  restores the blunt behaviour for a dedicated box with nobody at it.
- **Seed the portable config FROM the user's real one — never from empty.** The
  throwaway `reaper.ini` is what keeps a run from polluting somebody's install, but an
  *empty* one makes REAPER come up as a fresh install on every launch: licence prompt,
  first-run preferences, audio-hardware setup dialog, over and over. Copy the real
  config and licence in, override only the plugin-scan paths, and never write back.
  Two specifics that bite:
  - **Force `lastproject=`/`loadlastproj=0`.** Seeding carries `lastproject`, so the
    "isolated" REAPER opens the user's actual session — and if that session references
    a plugin the smoke did not install, REAPER sits on a modal *Project Load Warning*
    forever. The scripted insert never runs and the smoke reports INCONCLUSIVE naming
    neither the dialog nor the project, while a stranger's work is on screen.
  - **Strip `[Recent]`.** It is not needed to run a smoke, and copying it puts the
    user's project filenames into a temp dir and into REAPER's menus for the run.
- **AU cannot be proven over SSH — and the failure is silent.** An AU is found through
  the system AudioComponent registry, which is not visible to a process outside a GUI
  login session. Over SSH the component installs correctly and is completely
  invisible: `auval -a` lists nothing, REAPER's scan finds nothing, and the run ends
  INCONCLUSIVE — which reads as flakiness and sends the next person hunting a scan bug
  that does not exist. Detect `SSH_CONNECTION`/`SSH_TTY` and SKIP with that reason.
  VST3 and CLAP are unaffected: they are scanned from a directory.
- **An already-installed AU is the normal case, not a collision.** Refusing to run
  when `~/Library/Audio/Plug-Ins/Components/<name>` exists makes the AU leg unrunnable
  on every machine anyone would actually test on. If the installed component resolves
  to the same path as the one under test, use it in place and leave the "we installed
  it" flag unset so teardown does not uninstall something this run did not install.
  Only a *different* component at that path is a real collision.
- **Qualify the FX name by format, or the run proves the wrong plugin.**
  `TrackFX_AddByName` with a bare name lets REAPER pick whichever format it finds
  first, so `--format clap` inserted the installed **AU** and the run reported a CLAP
  pass for an AU — caught only by a crash report naming the `.au` bundle during a clap
  run. Prefix the name with `VST3:` / `CLAP:` / `AU:`. Three legs that all load the
  same plugin are one leg wearing three hats.
- **Prefer a trigger FILE over driving the screen — this is the settled answer.**
  Synthetic clicks and keystrokes typed a generation prompt into somebody's terminal
  *twice*, because every guess about what is on screen finds a new way to be wrong.
  The claim `editor-build` proves — that the generator runs when the *plugin* spawns
  it, inheriting the host's environment — is just as true asked through a file as
  asked through a button, so `editor-build` asks through a file and touches nothing.
  If you are about to add screen-driving to a mode, reach for a file handshake first.
  The two traps below are why, and they still apply to any mode that genuinely must
  drive the UI:
  - **Never trust "frontmost"; ask the window server who owns the pixel.** REAPER
    reports itself frontmost while its editor sits under a remote-desktop session or a
    terminal full of live agent sessions, so clicks and typed text land there. On a
    shared machine that is not a failed test — it is typing into somebody's work.
    Probe the actual owner at the exact points you are about to click. A
    pixel/brightness heuristic is worse in the other direction: it refused a plainly
    visible editor because the panels were lighter than the surface it expected.
  - **The hosted editor is not the host window.** REAPER wraps a plugin editor in a
    preset strip and pads it, so window-relative fractions land in the chrome — one
    Build click hit the Module tab and started a module build nobody asked for.
    Measure the plugin's surface inside the window rather than assuming an offset.
- **Dismiss `screencaptureui`, not "Screenshot".** `screencapture` leaves the capture
  UI running with a floating window that then sits *over* the plugin editor, so a run
  that failed and grabbed a screenshot arms the obstruction that blocks the next run.
  Quitting the Screenshot *app* does not touch it — `pkill -x screencaptureui` does.
- **Never pollute the user's machine.** VST3/CLAP scan from a TEMP path (portable
  `reaper.ini` `vstpath`/`clap_path`), never `~/Library/Audio/Plug-Ins`. AU can't be
  scanned from a custom path, so `--format au` installs to the real Components folder
  and MUST uninstall on exit. Cleanup ALWAYS runs (finally): kill REAPER, rm temp
  dirs, uninstall AU.
- **The temp-path scan does not actually work — `--format au` is the only lane that
  tests anything.** Measured 2026-07-30 on REAPER/macOS arm64: REAPER **ignores** the
  `vstpath`/`clap_path` keys written into the portable `reaper.ini`. It scanned the
  real plugin folders instead (124 CLAP + 104 VST3 entries in the portable caches)
  and never looked at the staged copy, so the bundle under test is absent from the
  scan cache and the FX is never found. Because a not-found FX is INCONCLUSIVE rather
  than FAIL, this degrades **silently**: the run reports "flaky launch" forever while
  testing nothing. Confirm a CLAP/VST3 run really scanned your plugin by grepping the
  portable `reaper-clap-*.ini` / `reaper-vstplugins_*.ini` for its name before
  believing any verdict. Until the key names are fixed, use `--format au`.
- **SKIP is never PASS.** Exit codes: 0 PASS / 1 FAIL / 2 SKIP (REAPER absent) /
  3 INCONCLUSIVE. A gate must treat SKIP/INCONCLUSIVE as not-passed.
- **A4 DPR v2 evidence requires all three format-qualified editor passes for
  every Forge DAW cell:** AUv2 in Logic, plus VST3 and CLAP in REAPER. Bind each
  subreceipt to the exact A3 lifecycle, plugin binary, host, and aggregate DPR
  matrix cell. All three must agree on terminal verdict, gates, and identity;
  their metrics remain independently retained. SKIP/INCONCLUSIVE,
  absence from the portable scan cache, or a different format being inserted
  leaves the aggregate cell incomplete. Keep the file-handshake rule above; DPR evidence
  does not authorize screen-driving.
  Drive or ingest that cell through `tools/scripts/gpu_dpr_runner.py`; the DAW
  adapter must emit the exact format, binary, scan-cache, raw-sample, artifact,
  and machine receipts. Metrics must retain measured/derived/unavailable
  provenance, and the input expectation must come from the frozen scenario—not
  from the DAW producer that reports the observed event/hit. Two same-content
  image artifacts and numeric fidelity oracles replace producer-authored pass
  booleans. Timeout is INCONCLUSIVE and durable for resumption.
- **REAPER license is a secret** (`~/.config/pulp/secrets/reaper-license.txt`,
  personal non-commercial) — never commit, echo, or bake into a TartCI golden. Local
  Macs (m1/m3/m5) are already serialized; local is the primary lane.

## GPU-health DAW evidence

For a product declaring `gpu.health.read`, bind the response to the exact DAW
instance/publication and preserve the returned snapshot. A valid response is
not automatically a passing startup trial. A visible DAW campaign must declare
`native-compositor-presentation` and bind an independent compositor timestamp;
capture completion cannot substitute. Missing source/shader or stage timings
are instrumentation coverage rather than event loss and remain nullable only
under the closed A3 verifier's passing no-change or failing investigation rules.
Preserve the correlated A2T trace. Produce the seeded blank receipt with the
focused real-Standalone test command in
`docs/validation/gpu-first-visible-a3-acceptance.md`; merely setting
`PULP_GPU_HEALTH_SEED_BLANK_FRAME=1` does not bind a control artifact. Keep
SKIP/INCONCLUSIVE and absent format scans as pending, never PASS.

Use `reaper_smoke.py --mode editor-open --reaper-bin /exact/REAPER` as the exact-format prerequisite for a
DAW A3 adapter, then execute that adapter through
`gpu_first_visible_a3_campaign.py run-role --role daw`. The smoke result cannot
be copied into the campaign: the adapter still owes 10 real cold and 10 real
warm editor lifecycles, independent native presentation timing, exact product
and host artifacts, the same-instance trace, and durable cache provenance.
Translate REAPER's `--format au` identity to the receipt's `auv2`; VST3 and CLAP
retain their names. An adapter timeout, SKIP, or INCONCLUSIVE produces a
nonterminal `run.json`, never a passing campaign.

Use the checked-in `gpu_first_visible_a3_external_adapter.py` as the immutable
envelope and select `gpu_first_visible_a3_reaper_producer.py`. Configure its
exact source-bound smoke/Lua helpers, REAPER, plugin binary/bundle, role driver,
checked-in source-bound trace analyzer, embedded-build verifier, and
digest-bound product build attestation/receipt. Also configure a reviewed
source-build driver from the exact clean revision; it must reproduce the
measured plugin executable and complete bundle in a fresh output without being
given the measured path. The analyzer is a sealed prepared binary, and every
REAPER lifecycle answers the producer's live-host nonce with exact executable
and start identity;
the driver must resolve to the reviewed file at the requested clean Pulp
revision. The producer runs the qualified-format preflight and then requires
the driver to perform all 20 editor opens, bind the trace to one challenged
live REAPER PID, and terminate every reported REAPER PID. It retains and rehashes those inputs with the
exact driver request/receipt in the host evidence tar. Missing automation,
build provenance, analyzer replay, or native-present truth is nonterminal;
the producer cannot promote the `editor-open` smoke or a screenshot completion
into campaign evidence.

The measured plugin executable must carry the canonical embedded build marker,
and the source-bound analyzer must preserve structural `unverified` separately
from the campaign budget verdict. Before terminal A3, also run the four-state
product trace-producer overhead protocol in the acceptance guide. Its exact
pre-change, compile-out, compiled-in idle, and active measurements require zero
xruns/audio-thread trace events; the DAW role campaign alone does not waive it.

For the DAW-selected overhead row, run the same reviewed REAPER lifecycle
driver through `gpu_first_visible_a3_trace_producer_overhead.py collect-state`;
do not time the smoke wrapper or relabel its editor-open result. Every one of
the 55 samples/state must answer the collector while the challenged REAPER PID
and exact plug-in executable are alive, then terminate cleanly. Active replay
must find the health producer and bind the complete b4ba exact 20-signature
`state`/`render`/`js` package from the A3 acceptance guide.
`gpu_acquire`/`gpu_submit`/`gpu_present` are mandatory per sample; every other
signature remains counted and a zero count is reported not-covered, not
zero-cost. Require zero xruns and zero producer events on the audio TIDs.
Pin the candidate-relative `state_build_driver` too. The collector exports and
default-deny rebuilds the exact source row without the measured plug-in,
ambient build tree, or network, then requires identical executable bytes and
the requested tracing sentinel state. Keep its source/build/toolchain artifacts
with the campaign; an already-built bundle or receipt assertion is not proof.
