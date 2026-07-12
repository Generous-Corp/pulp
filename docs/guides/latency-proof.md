# Proving reported latency

A plugin tells the host how many samples it delays audio by. The host slides the
whole track back by that much so everything lines up again. Nothing ever checks
that the number is true.

This guide is about the tool that checks it, and — just as importantly — about
the fact that **most plugins do not need it.**

---

## The bug it catches

`Processor::latency_samples()` is a promise. The host acts on it blindly: it
believes you, shifts the track, and moves on. Plugin Delay Compensation is a
system built entirely on trust.

So if the number is wrong, nothing errors. Nothing turns red. The plugin still
loads, still passes validation, still sounds fine in isolation. What happens
instead is that your track is now misaligned against every other track in the
session by the size of your mistake:

- **In parallel processing**, a wrong report is the worst case. The dry and wet
  copies of the same signal are no longer time-aligned, so summing them comb
  filters — a hollow, phasey, "something is off" sound that people chase for
  hours and usually blame on the plugin's tone.
- **In a multi-mic setup**, the mic you processed drifts out of phase with the
  mics you didn't.
- **In a mix**, everything just smears slightly, and nobody can point at why.

The failure is silent, it is downstream, and it does not reproduce in the
plugin's own UI. That is what makes it worth a tool.

## What it actually proves

Render the plugin, then check the audio against what the plugin claimed:

> The plugin says it delays by N samples. The audio is delayed by M samples.
> Do N and M agree?

That is the whole idea. It is a **self-consistency** proof: the report matches
the audio. That is precisely the property the host depends on, and it is the one
nobody was checking.

Be clear about what this does **not** prove: it does not prove N is the latency
you *intended*. A plugin whose true delay and whose report both grew together is
still perfectly compensated — it sounds correct — it just quietly got slower.
See [Pinning the value you meant](#pinning-the-value-you-meant) below for that.

---

## When to use it

Honestly: this is a sharp tool for a narrow class of plugin. Reach for it if
your plugin delays audio for a *structural* reason:

| Kind of DSP | Why it has latency |
|---|---|
| Convolution / partitioned convolution | Fills a block before it can transform it |
| FFT / STFT / spectral | Waits for a full analysis window |
| Lookahead limiters, gates, compressors | Delays the signal so it can see the future |
| Oversampling | Filter group delay on the way up and back down |
| Neural / block-inference | Batches samples before it can run inference |
| Linear-phase EQ | FIR group delay |

The common thread: **the latency is a consequence of the architecture, so it
moves when the architecture moves.** Someone doubles an FFT size, changes a
partition scheme, adds an oversampling stage — and the reported constant is a
separate line of code that they have to remember to update. That is the whole
bug class, and it is exactly the kind of thing a test should hold down.

It is worth the most when the latency is *derived* rather than fixed: computed
from FFT size, block size, filter length, or an engine mode that can change at
run time.

## When NOT to use it

**Most plugins have zero latency, and for them this tool has nothing to say.**

To put a real number on that: of Pulp's 24 example plugins, exactly **three**
override `latency_samples()` at all, and only **two** ever report nonzero
(`SuperConvolver` and `SpectralLab` — both convolution/spectral). Every other
example — gain, EQ, compressor, distortion, delay, the synths, the samplers —
correctly reports zero, and a test that renders them to prove "0 == 0" is
ceremony, not evidence.

Skip it when:

- **Your plugin is sample-in, sample-out.** Gain, EQ, waveshaping, a compressor
  without lookahead, a delay line (a delay *effect* has no *latency* — it emits
  its first sample immediately). Nothing to prove.
- **Your plugin is an instrument.** There is no input to compare the output
  against, so the delayed-null policy has nothing to null and there is no
  through-path to measure. You can still use the marker policy against a
  note-triggered onset, but a synth's reported latency is nearly always zero and
  nearly always trivially right.
- **You already have a golden-file test through the same path.** If a bit-exact
  reference render already covers the plugin, a latency drift would move the
  golden file and you would already know.
- **The number is a hardcoded literal that nothing derives.** `return 0;` does
  not need a render to prove.

If you read that list and concluded your plugin doesn't need this — good. That
is the correct outcome for most plugins, and it is why this is opt-in.

---

## How to run it

There is nothing to enable or disable. It does not run unless you ask for it: in
C++ it is a helper you call, and on the CLI it is a flag you pass. A plugin that
never asks for a latency proof is not affected by any of this, and there is no
runtime cost in a shipped build — the analysis library is a test/tool-tier
target and is never linked into a plugin.

### The demo, on a real plugin

`SuperConvolver` is a partitioned convolution reverb with genuine latency. Build
it and prove its claim:

```bash
cmake --build build --target SuperConvolver_CLAP -j"$(getconf _NPROCESSORS_ONLN)"

pulp audio render \
  --plugin "build/CLAP/SuperConvolver.clap" --format clap \
  --input-signal noise:9 --duration-frames 32768 --block 512 \
  --param 1=0 \
  --out /tmp/sc.wav --latency-report /tmp/sc.json
```

`--param 1=0` sets Mix to 0% — fully dry. The delayed-null policy needs the
plugin in a pass-through mode so the output *can* be a delayed copy of the
input; a fully-wet reverb tail is not a delayed copy of anything.

That render exits `0` and writes this artifact — verbatim:

```json
{
  "schema_version": 1,
  "report_status": "available",
  "reported_samples": 1536,
  "final_reported_samples": 1536,
  "report_observation": "stable",
  "observation_mode": "per_block_poll",
  "policy": "delayed_passthrough_null",
  "measurement_status": "match",
  "measured_samples": 1536,
  "delta_samples": 0,
  "tolerance_samples": 0,
  "null_depth_db": -200,
  "ambiguity_margin_db": 202.8781689626588,
  "contract_outcome": "satisfied",
  "gates_failure": false,
  "reason": ""
}
```

The plugin claimed 1536 samples. The audio is delayed by 1536 samples. Proven,
against a real bundle, with no DAW and no audio device.

### It refuses rather than guesses

Now run the same command with `--param 1=100` — fully **wet**. The output is a
reverb tail, not a delayed copy of the input, so there is no honest delay to
measure. The tool exits `1` and says so:

> output is not a delayed copy of the input (best residual 0.24272 dB at delay
> 1937, needs <= -60 dB). The delayed-null policy requires the processor to be
> in a declared identity / bypass / fully-dry mode.

Note what it did **not** do. Its search still found a best-scoring delay — 1937
samples — and that number is garbage. A tool that reported it would have been
confidently, plausibly wrong, which is worse than useless. Instead it checked
whether the answer was good enough to believe, decided it wasn't, and refused.

**An unprovable claim is a failed claim.** `inconclusive` exits non-zero exactly
like `violated` does. This tool will never hand you a latency it isn't sure of.

### In a test

```cpp
#include "support/audio_contracts.hpp"

TEST_CASE("my spectral plugin reports its true latency", "[latency]") {
    auto result = RenderScenario(create_my_plugin)
        .sample_rate(48000.0)
        .block_size(512)
        .duration_frames(32768)
        .input(make_white_noise(2, 32768, /*seed=*/0x51EED, 0.5f))
        .render();

    const auto evidence = evaluate_reported_latency(result);
    INFO(latency_evidence_summary(evidence));
    REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
}
```

`test/test_latency_contract.cpp` runs exactly this against a real
`SpectralFrameEngine` (Pulp's STFT engine) in identity mode, whose true latency
is `fft_size + analysis_hop` = 2560 samples. It proves the contract holds on DSP
that genuinely buffers and reconstructs a signal, not just on a delay line — and
a companion test deliberately misreports by one analysis hop and confirms it is
caught.

### From an agent (MCP)

`pulp_audio_render` takes `latency: true`, plus `latency_policy`,
`latency_tolerance`, `latency_intrinsic`, and `latency_expect`. It returns the
evidence, and an unproven or disproven claim comes back as an **error** — an
agent cannot mistake it for a pass.

---

## The two policies

**`delayed-null`** (use with `--input-signal noise`) sweeps a candidate delay D,
subtracts `input[n - D]` from the output, and takes the D that nulls best. It
checks every sample, so it catches a plugin that is delayed *and* subtly wrong.
It requires a pass-through/dry mode.

**`marker`** (use with `--input-signal impulse`) finds the single onset in the
output and subtracts its position in the input. Use it when the plugin *reshapes*
the signal, so nulling is meaningless. It is weaker: it only looks at one event,
and it depends on an onset threshold.

Use `--latency-intrinsic` with `marker` when the plugin adds delay that is not
*latency* — leading silence baked into a known IR, say. That is delay you do not
want the host to compensate, so it is subtracted before the comparison.

## How accurate is it, really?

**The delayed-null policy is sample-exact, or it refuses.** It does not
interpolate or estimate; it evaluates integer delays and picks one. The default
tolerance is 0 samples, and that is a realistic default — a correct plugin
matches exactly.

Two things bound it, and both fail closed:

**It must null deeply enough.** The residual at the winning delay has to sit at
or below −60 dB relative to the input, or the tool concludes the output isn't a
delayed copy at all and refuses (that is the wet-reverb case above). That −60 dB
floor is set for *real* DSP, not for bit-exact wires. Measured: the STFT engine's
overlap-add reconstruction — which is exact only up to float rounding — still
nulls to **−137 dB**. (A pure delay line reads −200 dB, which is not a
measurement but a clamp: its residual is exactly zero, and −200 is the reported
silence floor.) Both clear the bar by a wide margin. If your DSP genuinely cannot
null below −60 dB in its own dry mode, use the marker policy instead.

**The delay must be unambiguous.** A stimulus that repeats every P samples nulls
just as well at delay D as at D+P, so the delay is not recoverable and any answer
is a coin flip. The tool detects this: if a competing delay nulls within 12 dB of
the winner, it refuses. This is why the stimulus must be broadband and aperiodic
— use `noise` or `impulse`, never `sine` (the CLI rejects a sine under
delayed-null for exactly this reason). Measured margins on real runs are 140 dB
(STFT) and 203 dB (delay line), i.e. nowhere near ambiguous.

The evidence artifact reports both numbers — `null_depth_db` and
`ambiguity_margin_db` — so you never have to take the verdict on faith. A
measurement that passed with a 2 dB margin technically cleared the bar and is
still one small change away from being a coin flip; the artifact lets you see
that, and an agent should treat it as a finding.

**The marker policy is not sample-exact in the same way.** It depends on an onset
threshold, so a slow attack or a plugin that smears the transient can shift the
detected onset by a few samples. Give it a tolerance rather than demanding zero.

**What none of it can tell you** is whether the latency *should* be that value.
See below.

---

## Pinning the value you meant

Everything above proves self-consistency: the audio is delayed by exactly as much
as the plugin says. A host asks for nothing more, and a plugin that passes is
correctly compensated.

But consider: someone doubles an FFT size. The true delay grows by 2048 samples.
`latency_samples()` derives from the FFT size, so it dutifully reports the new,
larger number. The contract passes — honestly! The report *does* match the audio.
And your plugin just silently added 43 ms of latency to everyone's session.

Self-consistency cannot see that, because nothing is inconsistent. To catch it,
pin the value you meant:

```bash
pulp audio render ... --latency-report /tmp/l.json --latency-expect 1536
```

If the plugin is self-consistent but the value drifted:

> latency is self-consistent at 1536 samples, but the expected value is 512
> (drift +1024). The processor and the host agree with each other — the delay
> itself changed.

This is the check you want in CI on a plugin whose latency is derived from
geometry that people edit. It is a deliberate, separate opt-in, because a value
pin is only meaningful when there *is* an intended value — and it never masks the
more fundamental finding: if the audio and the report already disagree, you are
told *that*, not the pin.

---

## Where this lives

The analysis code is `tools/audio/analysis` (`pulp-audio-analysis`), a
**test/tool-tier static library**. It is deliberately not part of `core/` and is
never linked into a plugin or a runtime build — proving a claim about a plugin is
not something a plugin should carry code to do. Both the `pulp` CLI and
`test/support` link it, which is why the same evaluator backs the CLI flag, the
C++ test helper, and the MCP tool. One implementation, three surfaces, one
verdict.

**One honest gap:** because it is tool-tier, it is not installed with the SDK. A
downstream project consuming Pulp as an SDK can use `pulp audio render
--latency-report` (the CLI ships), but cannot currently `#include` the C++
evaluators and call `evaluate_reported_latency()` from its own test suite. If you
want that, say so — it is a packaging decision, not a technical obstacle.

## See also

- [`pulp audio render`](../reference/cli.md) — full flag reference
- `.agents/skills/audio-harness/SKILL.md` — the agent-facing workflow
- `test/test_latency_contract.cpp` — the contract's own tests, including the
  SpectralFrameEngine case
