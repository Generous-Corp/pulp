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
