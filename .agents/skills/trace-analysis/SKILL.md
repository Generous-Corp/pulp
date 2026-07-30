---
name: trace-analysis
description: The investigation harness for "why is this slow?" over a Pulp Perfetto trace (.pftrace). Runs the hypothesis→query→drill-down chain-of-evidence loop autonomously and returns a plain-English root cause + evidence + a concrete fix. TRIGGER on "why is my plugin slow to open", "find the slowest frames", "why is the UI stuttering", "why is my plugin using so much CPU", "which DSP node is expensive", "the load meter looks calm but CPU is pinned", or any `pulp trace explain "<question>"` / `/trace "<question>"` / `pulp_trace_explain` invocation. Ships Pulp-specific hints for dsp, frame, js, gpu, and cross-platform symptoms.
requires:
  - .agents/skills/trace-analysis/references/hints_dsp.md
  - .agents/skills/trace-analysis/references/hints_frame.md
  - .agents/skills/trace-analysis/references/hints_js.md
  - .agents/skills/trace-analysis/references/hints_gpu.md
  - .agents/skills/trace-analysis/references/hints_crossplatform.md
---

# trace-analysis — the "why is this slow?" investigation harness

Someone who has never opened a profiler types one line — *"why is my plugin
slow to open?"* — and gets a real, plain-English answer with a suggested fix.
This skill is the protocol that makes that happen: you load it, capture or
accept a `.pftrace`, and run a disciplined investigation over it, returning a
narrated root cause and a chain of evidence — never raw SQL. **You are reading
this because a tracing question fired (L1 `explain`) or an expert is driving an
iterative investigation (L2).**

This skill is provider-agnostic by construction: it is a plain `SKILL.md` read
identically by **Claude Code and Codex** from `.agents/skills/`. The whole
"why is this slow?" experience works under any agent because it is a shipped
skill + CLI/MCP surface, not a vendor API.

Its companion is `trace-sql` — the SQL discipline and the Pulp trace-stdlib of
named query primitives. This harness decides *what to ask*; `trace-sql` is
*how to ask it*. Load both for an L2 session.

> **Attribution.** The investigation methodology below — the chain-of-evidence
> scratchpad, the hypothesis→query→drill-down loop, the wall-time-vs-CPU-time
> rule, follow-the-blocker, exhaustive global verification, and p95/p99
> discipline — is **adapted from Google's `android/skills`
> `perfetto-trace-analysis` (Apache-2.0)**. The methodology is reused; the
> Android domain content (SurfaceFlinger, binder, RenderThread, ftrace, cpu
> governor) is not — the Pulp domain hints in `references/` are authored fresh
> against Pulp's own seams. See NOTICE.md.

---

## The tiers (where this skill sits)

| Tier | Who | Entry | This skill's role |
|---|---|---|---|
| **L0** | novice, no agent | `pulp trace slowest-frames`, `--preset dsp-hotspots`/`xruns` | not needed — canned preset → plain table |
| **L1** | novice, one-shot | `pulp trace explain "<q>"` · `pulp_trace_explain` · `/trace "<q>"` | **run this protocol autonomously**, return narrated root cause + evidence + fix |
| **L2** | expert, iterative | `pulp trace query "<sql>"` + this skill + `trace-sql` loaded | drive the full loop by hand on hard/multi-bottleneck cases |

L0 needs no agent. L1 is the headline: you run the whole protocol and hand back
prose. L2 is the same protocol, interactive, for cases the presets cannot crack.

---

## Capture (if you don't already have a `.pftrace`)

If a capture or query fails for an unclear reason, run the readiness check first
— it tells you, in one shot, whether the inspector is reachable, whether the
host was built with `-DPULP_TRACING=ON`, and whether a `trace_processor` is
available for offline SQL:

```bash
pulp trace doctor            # human report; add --json for {ready_to_capture, ready_to_query, …}
```

`ready_to_capture:false` usually means no eligible inspector session was found
or tracing was compiled out. Normal launches create no endpoint; capture
requires an explicitly wired custom fixture published through authenticated discovery.
`ready_to_query:false` means no `trace_processor` (on
`$PULP_TRACE_PROCESSOR`, the pinned Pulp-fetched build, or `$PATH`) or no
captured trace yet. For zero-install, run
`pulp trace fetch` once — it downloads the pinned `trace_processor_shell`
(Perfetto v57.2), SHA-256-verified, into `$PULP_HOME`. (`pulp tool install
trace-processor` fetches the same pinned artifact via the tool registry.)

```bash
pulp trace start --categories render,gpu,text,js,layout   # pick the categories the question implicates
# ... reproduce (open the editor, sweep the knob, run the offline render) ...
pulp trace stop                                           # → /tmp/pulp-<ts>.pftrace
```

Or accept a `--trace FILE.pftrace` the user hands you. Choose categories from
the question: startup → `render,gpu,text,js,layout`; DSP cost → `dsp,dsp.node`
(offline render); UI hitch → `render,layout,canvas,text,js,gpu` plus a
concurrent `pulp motion record` so the motion `trace_id` joins in.

**Before trusting the capture**, confirm it is not silently empty/truncated
(ring overflow → empty trace): `SELECT DISTINCT category FROM slice`. No rows,
or missing the category you asked for, means re-capture with a larger
`--ring-mb` or a shorter window — not "nothing was slow."

When you already have a flushed `.pftrace` and no live session (the common case
for a trace a user handed you), run SQL against the file directly — no inspector
needed:

```bash
pulp trace query "SELECT DISTINCT category FROM slice" --trace /tmp/pulp-<ts>.pftrace
```

This shells out to `trace_processor_shell` (`$PULP_TRACE_PROCESSOR` → pinned
Pulp-fetched build → `$PATH`; `pulp trace doctor` reports which, `pulp trace
fetch` installs the pinned one) and returns its native table. `--format`
json/csv and `--preset` apply only to the live inspector path; offline is raw
SQL against the file.

To eyeball a trace in the Perfetto timeline instead of querying it, hand it to
the UI (browsers block `file://`, so this serves it over loopback and opens the
UI at it): `pulp trace open /tmp/pulp-<ts>.pftrace` (`--no-browser` prints the
URL to paste; `--json` for agents).

---

## The investigation protocol

### 1. Keep a chain-of-evidence scratchpad
Write down, as you go: the question → your current hypothesis → the query you
ran → what it showed → the next hypothesis. Every claim in your final answer
must trace back to a specific query result. "Looks slow" is not a finding; "a
`text` span of 540 ms precedes the first frame, from query X" is.

### 2. Hypothesis → query → drill-down loop
Form one hypothesis at a time, query it via the `trace-sql` stdlib (start with
the named views), read the result, refine or discard. Drill from coarse to
fine: category totals (`pulp_layout_vs_paint`) → the fat span → the thread it
ran on → its children → its args. Do not write one giant query; walk down.

### 3. Wall time vs CPU time — the rule that prevents wrong answers
`slice.dur` is **wall-clock** duration. A long slice may be *blocked* (waiting
on a lock, I/O, the GPU, another thread), not *computing*. Before blaming a
long span, check whether it was actually running: join to `thread_state` /
inspect the thread's scheduling for that window. A 600 ms span that was
`Runnable`/blocked 90% of the time is a **waiting** problem (fix the blocker),
not a **compute** problem (optimize the code). Getting this backwards sends the
fix in the wrong direction.

### 4. Follow the blocker across threads
When step 3 says a span was blocked, **follow the blocker**: which thread /
resource held it? The audio block waited on a mutex the UI thread took; the
present waited on a GPU pass; the layout waited on a JS callback. Trace the
wait to its holder on the *other* thread and make *that* the next hypothesis.
The bottleneck is frequently not on the thread that looks slow.

### 5. Exhaustive global verification — do not stop at the first bottleneck
Finding one fat span is not the end. Verify globally: is this the *dominant*
cost, or one of several? Sum the category totals and check the found span's
share. Look for a second offender of comparable size. A fix that removes a
300 ms span from a 2.4 s open still leaves 2.1 s — say so. Report the full
budget, ranked, not just the first thing you noticed.

### 6. p95/p99 discipline — the mean lies
For anything recurring (per-block DSP, per-frame render), the **mean hides the
spike**. A node at 40% mean can eat 60% of the *worst* block. Look at `max_ms`
next to `mean_ms` (the stdlib views expose both), and when the gap is
ambiguous compute the tail (p95/p99) over the raw slices — see `trace-sql`.
"The load meter said 40%; the trace said WHY" is exactly this: the average was
calm, one node's tail was not.

The mean also lies in the *other* direction: a per-node average can be
dominated by a one-time **cold-start spike on the first block** (the first
`process()` call warms caches / touches fresh pages), making a genuinely cheap
node look like the worst offender. Always separate the first-block outlier
from steady-state cost — subtract the per-node `MAX(dur)` (the steady-state
query in `trace-sql`'s "Common query shapes"), or just read the flamegraph and
ignore block 0. `examples/trace-plugin-chain` is a runnable demonstration:
gain's whole-run average is ~158× its steady-state cost, and the real per-block
hot node is the biquad filter, not the gain the average fingers.

### 7. Consult the domain hints
Match the symptom to a hints file and read it before drawing conclusions — each
grounds the analysis in Pulp's real seams and names the specific traps:

| Symptom | Hints file |
|---|---|
| xruns, per-node DSP cost, deadline miss, "meter calm but one node dominates", jitter/denormals | `references/hints_dsp.md` |
| dropped frames vs vsync budget, layout-vs-paint, `TextShaper::prepare` re-runs, dirty-rect churn, GPU-submit stalls | `references/hints_frame.md` |
| QuickJS bridge dispatch cost, a JS callback invalidating layout | `references/hints_js.md` |
| Dawn submit/present stalls, Graphite record cost, per-pass GPU time | `references/hints_gpu.md` |
| standalone vs plugin-in-DAW vs iOS/iPadOS AUv3 vs Android/Oboe vs Simulator; sample-position args, thread naming, atrace interleave | `references/hints_crossplatform.md` |

### 8. Answer in plain English (L1) — never surface SQL
Return: **root cause** (one or two sentences), **chain of evidence** (numbered,
each tied to what a span/query showed), and a **concrete fix**. Give magnitudes
("~620 ms Dawn/Graphite init, one-time" ), say whether the cost repeats, and
estimate the win. See the worked narrative below.

### Escalating to the user — only at a genuine priority fork
**Default: investigate autonomously.** Follow the blocker, gather evidence, and
verify exhaustively. Resolve every *factual* gap from the trace itself — a
category total, a thread state, a span's args are things you query, not things
you ask. Never stop to ask the user something a query can answer.

**Reach for `AskUserQuestion` only when the direction turns on the user's
priorities, not on data you can gather.** Genuine forks: startup splits
near-evenly across font-shaping *and* shader-compile and only the user knows
which path matters for their use case; or the fix has a fast-approximate branch
and a slower-thorough branch and the tradeoff is theirs to pick. These are
preference forks, not missing facts.

**Form it well:** put the recommended option first and label it recommended,
with a terse pro/con per option. Never use it as a progress checkpoint ("should
I keep going?") — that is exactly the pause to avoid — and never to re-confirm a
decision already made or to ask what a query would answer.

---

## Worked example — "why is my plugin slow to open?" (the flagship)

This is the safest, most relatable case: bounded, one-shot, **main-thread**,
deterministic — no real-time hazard, works regardless of the DSP story.

```bash
pulp trace start --categories render,gpu,text,js,layout
# ... open the editor ...
pulp trace stop
pulp trace explain "why is my plugin slow to open?"
```

A good answer reads like this:

> **Root cause:** first editor-open spends ~2.4 s, and ~1.9 s of it is one-time
> GPU/text setup on the UI thread before the first frame — none of it cached
> between opens.
>
> **Breakdown:** Dawn device + Graphite context init ~620 ms, Skia font-atlas
> build for the UI typeface ~540 ms, QuickJS eval of the bundled UI script
> ~410 ms, first Yoga layout + `TextShaper::prepare` of every label ~330 ms.
>
> **Chain of evidence:** (1) a `gpu`/`render` span pair brackets the ~620 ms
> device init on the main thread. (2) `text` spans show the font-atlas build
> (~540 ms) preceding the per-label `TextShaper::prepare`. (3) the `js` span for
> script eval is ~410 ms, single-shot. (4) a second open repeats the same spans
> identically — nothing is reused.
>
> **Fix:** warm the Dawn/Graphite context and font atlas once per process (not
> per editor open) and cache the compiled JS module. Re-opens should drop from
> ~2.4 s to well under 500 ms.

The other canonical case is the offline DSP reveal — "CPU pinned but the meter
looks calm — which node?" — run against a deterministic `offline_process()`
render (`examples/trace-demo`) so the answer reproduces exactly. See
`hints_dsp.md` and docs/guides/tracing.md use case 3.

---

## Agent contract

1. State the question as a measurable target before querying.
2. Verify the capture is non-empty and has the categories you need before
   analyzing. An empty trace is a capture bug, not a "fast" program.
3. Distinguish wall time from CPU time before blaming a long span (step 3).
4. Follow blockers across threads; the slow thread is often not the guilty one.
5. Verify globally — report the ranked budget, not the first bottleneck.
6. Every claim cites a query result. No evidence, no finding.
7. Investigate autonomously; `AskUserQuestion` only at a genuine priority fork,
   never for something a query answers or a progress checkpoint.
8. L1 returns prose (root cause + evidence + fix); never dump SQL at a novice.

## Files this skill covers

- `.agents/skills/trace-analysis/references/hints_dsp.md`
- `.agents/skills/trace-analysis/references/hints_frame.md`
- `.agents/skills/trace-analysis/references/hints_js.md`
- `.agents/skills/trace-analysis/references/hints_gpu.md`
- `.agents/skills/trace-analysis/references/hints_crossplatform.md`
- `.agents/skills/trace-sql/SKILL.md` — the SQL substrate + trace-stdlib
- `core/runtime/include/pulp/runtime/trace.hpp` — macro surface + category taxonomy
- `docs/guides/tracing.md` — the guide, tiers (L0/L1/L2), worked use cases, gotchas

## Tracing a plug-in on Windows

Windows tracing was unusable until 2026-07-25 — four independent blockers, each
fatal on its own. If a Windows capture comes back empty, check these first.

1. **`PULP_TRACING=ON` did not compile under MSVC.** `trace.cpp`'s ship-guard
   sentinel used `__attribute__((used, visibility("default")))`; MSVC errors
   C4430/C2065/C3861 and `pulp-runtime` fails, taking 22 dependent targets with
   it. Now `__declspec(dllexport)` on MSVC.
2. **Perfetto was excluded from the installed SDK.** `pulp-runtime` linked
   tracing through `$<BUILD_INTERFACE:pulp-tracing>`, correct when tracing is
   OFF and wrong when ON — every library carries Perfetto symbols but the export
   named none, so a plug-in linking a traced SDK failed with unresolved
   `perfetto::` symbols. `pulp-perfetto`/`pulp-tracing`/`perfetto.h` now install
   into `PulpTargets` when tracing is ON.
3. **No code path ever started a session.** `Tracing::start()` had zero callers
   and a plug-in has no `main()`. `Tracing::attach()` now autostarts from
   `$PULP_TRACE_PATH`, and the VST3 adapter attaches/detaches over its lifetime.
4. **The Windows plug-in host had no spans.** Only `window_host_mac.mm` had
   `render/frame` + `canvas/paint`.

### Capture recipe

```bash
cmake -S <pulp> -B <build> -DPULP_TRACING=ON       # then build + cmake --install
# in the HOST process environment (not the build shell):
PULP_TRACE_PATH=C:\path\out.pftrace
PULP_TRACE_SECONDS=45      # timed flush; see below
```

`PULP_TRACE_SECONDS` matters. Perfetto's `duration_ms` only caps the buffer —
the `.pftrace` is written by `stop()`, which otherwise means unloading the
plug-in you are profiling. The timed flush makes a capture self-completing.

For an SDK-built plug-in, prove that the installed package was configured with
`PULP_TRACING=ON`; enabling the option only in the plug-in's consumer build
cannot reconstruct omitted Perfetto targets or headers. A valid traced SDK
exports the tracing support targets transitively, and the host process—not the
build shell—must receive `PULP_TRACE_PATH` and `PULP_TRACE_SECONDS`. If the
plug-in loads but produces no file, distinguish an untraced installed SDK from
a session that merely has not flushed before changing instrumentation.

### The auto-flush timer is owned, joined, and generation-tagged

`PULP_TRACE_SECONDS` used to arm a DETACHED `std::thread` that slept and
then called back into process-global tracing state. Two consequences you
may still see in older builds:

- **A capture that truncates early.** Close and reopen the editor inside
  the window and the FIRST session's timer stopped the SECOND session.
  Timeouts now carry the session generation they were armed for and
  refuse to act on any other, so a re-opened editor gets its full window.
- **A crash on plug-in unload.** Nothing joined the sleeping thread, so
  `FreeLibrary` / `dlclose` could pull the module out from under it. The
  final `Tracing::detach()` now cancels and JOINS the timer before it
  flushes.

Practical consequence for capture: the last detach is a synchronous
flush + join. If you are scripting a capture, let the host finish
unloading the plug-in rather than killing the process — a `SIGKILL`
still loses the trace, but a clean unload no longer races the timer.

Adapters attach via RAII (`runtime::ScopedTracingAttachment`), and
tracing is now wired into **VST3, CLAP, AU v2, AU v3, AAX, and
Standalone** — it used to be VST3-only, so a Perfetto capture of any
other format recorded nothing while the API claimed to be
process-global. If a capture is empty, check the format is one of those
before suspecting the environment.

### Always instrument the blocking call

A frame span whose children sum to ~2 ms while the frame itself takes 45 ms
means the cost is in an **uninstrumented** call inside it. On Windows that was
the swapchain acquire (`gpu_acquire`, added 2026-07-25): with a Fifo present
mode `GetCurrentTexture()` blocks until the next refresh. Before that span
existed the time had nowhere to be attributed and the trace looked healthy.

### Driving a Windows GUI capture with nobody watching

Screenshot/input automation needs an **Active** session; a disconnected RDP
session captures blank frames at the default 800x600. Move the session to the
console so it stays renderable with no client attached:

```powershell
tscon <session-id> /dest:console     # session stays Active, RDP client detaches
```

Pair with auto-logon so a session exists after reboot, and run long builds under
Task Scheduler (S4U) — an SSH drop otherwise kills `cmake`/MSBuild mid-build.

## A capture with NO render spans — read this before investigating

A trace containing `layout_children` and `wm_mousemove` but **no** `frame`,
`paint`, `gpu_acquire`, `gpu_submit` or `gpu_present` is the single most
misleading result this harness produces. It looks like a broken capture or a
frame-time regression and is usually neither. Work the ladder in order; each
step is seconds, and step 1 explains most cases.

### 1. Which host did the plug-in actually get?

The five render spans live on the **GPU** paint path. A plug-in that does not
ask for a GPU editor never enters it, paints correctly on CPU raster, and emits
none of them — by design, on every platform.

The adapter logs its decision at editor attach:

```
[plugin-gpu-host] adapter mode=autoui use_gpu=false wants_gpu=false
                  scripted=false requires_gpu_host=false …
VST3 editor: attached (536x230, mode=autoui, gpu=false)
```

`decide_gpu_host()` (`core/format/include/pulp/format/gpu_host_select.hpp`, no
platform guards — this is cross-platform) computes:

```cpp
d.wants_gpu = scripted || view_wants;      // view_wants = requires_gpu_host()
d.use_gpu   = d.wants_gpu && !env_off;
```

So `mode=autoui` with `wants_gpu=false` means the editor is neither scripted nor
declares `requires_gpu_host()`. **That is a complete explanation for zero render
spans.** Do not go looking for a fallback, a broken adapter or a regression: a
CPU-raster editor is not a degraded GPU editor, and its frame times are not
comparable to a GPU capture's. `mode=` is the first thing to read.

On Windows the CPU branch is explicit — `handle_wm_paint()` calls
`render_frame()` only `if (gpu_surface_ && skia_surface_)`, else
`raster_render_rgba()`, which carries no instrumentation and is labelled in
source as "the VM proof path".

### 2. Which host + platform emits which spans?

Coverage is **not uniform**, and only the Windows plug-in editor has the full
set. Verified by span-site inspection:

| host | `frame` / `paint` | `gpu_acquire` |
|---|---|---|
| Windows plug-in editor (`plugin_view_host_win.cpp`) | yes | yes (shared `PluginFrameRenderer`) |
| Linux plug-in editor (`plugin_view_host_linux.cpp`) | **no** | yes (shared renderer) |
| macOS plug-in editor (`plugin_view_host_mac.mm`) | **no** | **no** — it has its own `render_frame()` and no `PULP_TRACE` sites |
| macOS standalone app (`window_host_mac.mm`) | yes | no |

`gpu_submit` comes from `core/render/src/skia_surface*.cpp` and `gpu_present`
from `gpu_surface_dawn.cpp`, i.e. the render layer rather than the host, so they
can appear where the host-level spans do not.

The practical consequence: **do not read a missing `frame` span on a macOS
plug-in editor as a regression** — that host has never emitted one. Instrument
the host before measuring it, or measure the standalone app instead.

### 3. Read the plug-in's log before theorising

On Windows Pulp's log sink is `OutputDebugStringA`, which nothing captures by
default — so `[plugin-gpu-host]`, GPU-init failures and the CPU-fallback
diagnostic are all invisible unless you attach a listener FIRST.

There is no need for Sysinternals: a `DBWIN` listener is ~40 lines. Create the
`DBWIN_BUFFER` file mapping plus the `DBWIN_BUFFER_READY` / `DBWIN_DATA_READY`
events, then loop `SetEvent(ready)` → `WaitForSingleObject(data)` → read the pid
from the first 4 bytes and the message after it. Start it **before** the host
process and it captures everything from plug-in load onward. This is what turns
"no spans, cause unknown" into one line of fact.

On macOS the same information comes from `log stream` — see the `ios` skill for
a working predicate that includes `[plugin-gpu-host]`.

### 4. Only then suspect the capture

If `mode=scripted`/`custom` with `use_gpu=true` and the spans are still missing,
the earlier Windows blockers and the flush-lifetime notes above apply.

Related but different: the section below concerns `gpu_render_time`, an opt-in
timing COUNTER. Missing `gpu_render_time` and missing render SPANS have
unrelated causes; do not treat one as evidence about the other.

## GPU render time is now OPT-IN (WAH-13)

`SkiaSurface::gpu_render_timing_available()` reporting false is no longer
evidence of an adapter that lacks `timestamp-query`. Timestamps are requested
only when the host asks, via `PluginViewHost::Options::enable_gpu_timing`
(default OFF), rather than whenever the adapter advertises the feature.

That default is deliberate and worth understanding before you "fix" it: Dawn
gates `writeTimestamp` behind the `allow_unsafe_apis` toggle on every backend,
so requesting the feature forces that toggle on — and it applies to the DEVICE,
not to the diagnostic. Ordinary rendering was silently running with relaxed
validation on every machine whose adapter happened to offer timestamps.

If you need per-recording GPU time in a capture, enable it explicitly on the
host's Options. If a trace shows no `gpu_render_time`, check that flag before
suspecting the adapter.
