# A9 independent authoring trial packet

Status: prepared, not run. This packet is for a session that did not write any
of the graph code, the CLI or the docs it will read. The lead runs the trial;
the worker that wrote this packet must not.

## What the trial decides

Whether an independent agent, given only the public guides and the shipped
CLI, can author, validate, render, measure, edit and replay a small stereo
graph within the 200-line glue budget, and can show that the render is
non-silent. The earlier trial (2026-11-10 Codex session, and the 2026-11-13
Sonnet session) found `pulp graph render` took no input and wrote silent
WAVs for every `audio_in` graph. That defect is fixed on `origin/main`
(`pulp graph render --input`, `missing_input`, `invalid_option`, and a `peak`
field in the JSON), so the render criteria below can now be met.

## Preconditions the lead checks before launching

1. A fresh checkout or an SDK prefix at an `origin/main` head that contains the
   render fix: `git merge-base --is-ancestor 7b9bf31b1c21 <head>`.
2. Build the CLI from that head with
   `tools/ci/governed-build.sh cmake --build build --target pulp-cli pulp-rust-cli`.
   Record the head SHA and the SHA-256 of `build/pulp` and
   `build/tools/cli/pulp-cpp`.
3. Preflight, run by the lead (not the trial agent), must print a nonzero
   `peak`. This proves the instrument before the trial starts:

   ```sh
   pulp graph render first.pulpgraph --input in.wav --output out.wav --json
   ```

   using the files from `docs/guides/graph-first-steps.md`. A zero `peak` here
   means the build is stale; stop.
4. The trial agent is a different model family or tier from the author of the
   CLI and docs, in a new session with no memory of this program.

## Materials the trial agent may use

- `docs/guides/graph-first-steps.md`
- `docs/guides/graph-notation-cookbook.md`
- `docs/guides/graph-patch-notation.md`
- `docs/guides/graph-edge-probes.md`
- the `pulp graph` section of `docs/reference/cli.md`
- the built `pulp` CLI, offline, with plugin scanning disabled
- Python 3 standard library

## Materials it must not use

- any file under `core/`, `examples/`, `test/`, `planning/` or `tools/`
- `docs/graph-program-status/` (this file included) and `docs/evidence/`
- earlier trial outputs, including `trial.py` from prior runs
- the network and any ambient plugin

The lead hands over only the five documents and the CLI path. It does not tell
the agent the expected sample values.

## Prompt for the trial agent

Give this text verbatim:

> You are authoring a small audio graph with the `pulp graph` CLI. Use only the
> documents and CLI you were given, offline. Build a stereo graph that passes
> its input through one gain node of 0.5 to the output. Create a one-second or
> shorter stereo input WAV whose two channels hold different constant values.
> Then: (1) validate the graph and save it as `.pulpgraph`; (2) render it with
> your input and prove the rendered file is not silent and that each channel
> equals your own arithmetic prediction; (3) measure one named edge with a
> probe and show it agrees; (4) change only the gain to 0.25, render again, and
> show the output changed by the predicted amount; (5) re-render the original
> saved graph with the original input and show it reproduces the first render
> exactly. Work in one new directory. Deliver the files you authored, a short
> receipt listing every command you ran with its exit code, and the number of
> lines of code and notation you wrote. Stop when you have a coherent result or
> after 30 minutes, whichever is first, and report what you could not do.

## Pass criteria

Scored by the lead from the delivered files and by re-running the commands, not
from the agent's own summary.

| # | Criterion | Evidence |
|---|-----------|----------|
| 1 | Notation validates (`valid: true`) and saves | `pulp graph validate` and `save` exit 0 |
| 2 | First render is non-silent | lead reads the float WAV independently and finds a nonzero peak; the CLI `peak` must agree |
| 3 | Each channel equals the agent's stated prediction | lead recomputes from the input WAV: sample equals input times 0.5 within 1e-6 |
| 4 | Probe agrees with the render | probe `peak` on the named edge matches the expected edge value; `dropped` is 0 |
| 5 | Gain edit changes the output by the predicted factor | second render equals input times 0.25 within 1e-6 and differs from the first |
| 6 | Replay of the saved original reproduces the first render bit for bit | byte-equal WAV data chunks |
| 7 | No forbidden material was read | lead inspects the transcript for opened paths |
| 8 | Total authored glue is at most 200 lines | counting rule below |

Verdicts: PASS when 1 to 8 hold. PARTIAL when 1, 2 and 6 hold but a later
criterion fails or the glue is between 201 and 300 lines. FAIL when the render
is silent, a criterion fails for a tool defect, or the glue exceeds 300. A FAIL
or PARTIAL names each failing criterion with the exact command and output, and
whether the cause is a tool defect, a documentation gap, or the agent.

## Glue counting rule

Count every line the agent authored in delivered files that is non-blank and
not a comment-only line: scripts, notation (`.pat`), and any helper. Do not
count generated files (WAVs, `.pulpgraph` saves, probe or render JSON, the
receipt). Report the script lines and the notation lines separately and their
sum. The earlier Python-only trial authored 56 lines, so the budget is not
expected to bind; the number is recorded so a future regression is visible.

## What the lead records

- agent model name and tier as the session exposes it; mark "not exposed" if it
  does not rather than guessing
- wall-clock start of authoring and of the final replay, and per-command times
- head SHA, CLI hashes, Python version
- every documentation page the agent opened, and every error it hit, with the
  raw text
- the verdict table above, with the failing command for each miss

## Reference measurement for comparison

The in-repo reference consumer, `examples/graph-agent-loop`, is 165 lines of
authoring glue plus a 213-line verification harness, against 690 lines before
the split. It is a C++ installed-SDK consumer, a different path from this
CLI-only trial; the trial is not expected to match its line count.

## Non-claims

A PASS shows that one independent agent reached a correct, non-silent result
through the documented CLI path on one machine. It does not prove model
execution beyond that session, CLI and MCP parity, installed-SDK C++
authoring by an independent agent, realtime behavior, hosted plugins, or
completion of the A9 flagship.
