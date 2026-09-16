# Measuring a plugin without listening to it

Pulp is a C++20 framework for building audio plugins and applications. This
guide is about a narrower thing: the measurement machinery around it, and the
fact that all of it is reachable from a command line.

That matters for a specific reason. The usual loop for evaluating a DSP change
is to build it, load it into a host, play something through it, and listen —
then change a coefficient and do it again. The listening is the part that does
not scale and does not automate, and it is also the part that carries the
judgment. What follows does not replace the judgment. It replaces the loop
around it: generate a stimulus, push it through the plugin offline, measure the
result, read a number, decide, change the DSP, re-run. Every step in that
sentence is a process you can spawn and a number you can parse, so a script — or
an agent — can sit in that loop and only surface the cases where the number
moved.

The argument only holds if the numbers mean something. So every instrument
below is described together with what it cannot see. That is not hedging; it is
the load-bearing half. An instrument that returns a confident number for a
condition it cannot resolve is worse than no instrument, because it converts a
silent failure into a green one. The last section of this guide is a set of
real cases where exactly that happened here.

A note on vocabulary, since almost none of it is standard. A **`Processor`** is
the class you subclass to write DSP — it has a `prepare()` for allocation and a
`process()` for the audio callback, and format adapters wrap it to produce
VST3, AU, and CLAP builds. A **plugin bundle** is one of those compiled
adapters on disk. A **gate** is a check that blocks a merge. Everything else
gets introduced where it appears.

---

## The loop, end to end

Start with the shape of it, then the pieces. Two commands render a reference
and a candidate under an identical stimulus; a third measures the difference.

```bash
# Render the plugin you are comparing against, offline. No DAW, no audio device.
pulp audio render --plugin Reference.vst3 --format vst3 \
    --out /tmp/ref.wav --duration-ms 2000 \
    --input-signal sine:1000,-6 --sample-rate 48000 --block 128 \
    --wav-format float32 --json

# Render your candidate through exactly the same stimulus.
pulp audio render --plugin Candidate.clap --format clap \
    --out /tmp/cand.wav --duration-ms 2000 \
    --input-signal sine:1000,-6 --sample-rate 48000 --block 128 \
    --wav-format float32 --json

# Measure. Deterministic, in-process, no Python:
pulp audio validate compare /tmp/ref.wav /tmp/cand.wav --mode null --tolerance -100
```

`pulp audio render` loads a bundle through Pulp's plugin host and runs it
block by block against a generated or supplied input. `--input-signal` takes
`silence`, `sine:<hz>[,<dbfs>]`, `noise[:<seed>]`, or `impulse[:<frame>]`; the
noise seed is explicit precisely so a run reproduces. `--param <id>=<value>[@frame]`
sets or automates a parameter, and the values are in the plugin's own units,
not normalized 0–1. `--midi note:<note>,<vel>,<on>[,<off>]` drives an
instrument. `--json` puts the metrics on stdout and `--manifest <file.json>`
writes them to a file; either way what comes back is the same metrics object
that `pulp audio validate summarize --json` produces, so the render step and
the measure step speak one schema.

**What `pulp audio render` cannot do: it is bundle-only.** `--plugin` takes a
compiled VST3, AU, AUv3, CLAP, or LV2 on disk, and there is no flag that
accepts a `Processor` living in your source tree. If the DSP you want to
measure has not been built into a bundle yet, the CLI cannot reach it, and you
want the in-process path described further down instead.

`pulp audio validate` is the measuring end, and it has four verbs:

```bash
pulp audio validate summarize <file.wav> [--json]
pulp audio validate doctor    <file.wav> [--thd] [--response f1,f2,...] [--fundamental <hz>] [--channel <n>]
pulp audio validate compare   <a.wav> <b.wav> [--mode null|spectral] [--tolerance <dbfs>]
pulp audio validate assert    <dir-or-assertions.json>
```

The exit codes are the part that makes this scriptable, and they distinguish
three states rather than two: **0** means the check ran and passed, **1** means
it ran and failed (or errored), and **2** means it could not measure. That third
code is the one worth wiring into a script, because "I could not measure this"
and "this is fine" are the two outcomes that otherwise look identical.

### Looking at a window of the signal

```bash
pulp audio scope --input-wav /tmp/cand.wav --window 2048 \
    --trigger rising-zero --channel 0 --json /tmp/scope.json --png /tmp/scope.png
```

The PNG is for a human; the JSON is the one a script reads. It carries the
captured window plus a measurements block: peak-to-peak, RMS, DC offset, crest
factor, and an estimated frequency with its period in samples.

Two things about it. First, every measurement is paired with an `available`
flag and the result carries a `warnings` list, so when the analyzer cannot
resolve a quantity it says so instead of returning a plausible number — the
frequency estimate in particular requires at least two rising-zero intervals
that agree within a tolerance before it will report at all. Second, **a scope
window is a window.** With `--window 2048` you are looking at roughly 43 ms at
48 kHz, positioned by the trigger. Anything that happens outside it is not
merely small in the result, it is absent from it, and a defect that only
appears after ten seconds of decay will never enter the frame.

One caution: the form above, with `--input-wav`, is offline and reads a file.
The *other* form of `pulp audio scope` — the one that takes a live target —
wraps the standalone app and **opens a real audio device**, which will make
sound on the machine it runs on. For unattended measurement, stay on the
file-reading form.

### Per-artifact comparison

For "did this change make it sound worse, and in what way", there is a separate
Python tool, the **quality lab**. It is an opt-in install rather than part of
the build:

```bash
pulp tool install audio-quality-lab
pulp tool run audio-quality-lab -- compare /tmp/ref.wav /tmp/cand.wav --profile added-hf --json /tmp/verdict.json
```

`compare` runs **one** axis per invocation, chosen by `--profile`, defaulting
to `tonal-balance`. The six axes, each naming the detector behind it, are
`tonal-balance` (spectral centroid), `added-hf` (an HF-fizz detector),
`noise-roughness` (harmonic-to-noise ratio), `graininess` (spectral flux),
`stereo-width`, and `transient-integrity` (transient sharpness). The verdict
vocabulary is `regression_suspected`, `material_change_detected`,
`no_material_change_detected`, `inconclusive`, and `invalid` — note that
"inconclusive" and "invalid" are first-class outcomes rather than errors, which
is the same instinct as exit code 2 above.

**What `compare` does not give you is a timestamp.** Five of the six axes are
global scalars over the whole file; `transient-integrity` is computed per onset
but still reports aggregates. Localized output — a per-frame curve and the
worst regions, exportable as listenable clips — comes from the `run` subcommand
with `--out-dir`, not from `compare`. And running several axes at once is the
`regression-net` subcommand's job: it takes a manifest of before/after WAV pairs
and runs each pair through a set of axes, reporting `REGRESSION`, `ERROR`,
`INCONCLUSIVE`, or `CLEAN`. It consumes WAV paths — it does not render them, so
the rendering step above stays yours.

---

## When the DSP is not a bundle yet

The CLI needs a compiled plugin. During development the thing you want to
measure is usually a `Processor` in the source tree, and for that the harness
lives in the test suite instead.

**`HeadlessHost`** (in `core/format/`, so it ships rather than being test-only)
drives a `Processor` through prepare-and-process with no window, no device, and
no host. **`RenderScenario`** is a fluent builder on top of it that produces one
render plus its metrics:

```cpp
auto result = RenderScenario(pulp::examples::create_pulp_gain)
    .name("pulpgain.minus6")           // provenance, recorded into artifacts
    .sample_rate(48000.0)
    .block_size(128)
    .input(make_sine(2, 24000, 440.0f, 48000.0, 0.25f))
    .set_param(pulp::examples::kOutputGain, -6.0f)
    .render();

CHECK(assert_not_silent(result.metrics).passed);
CHECK(assert_frequency_near(result.output.channel(0), 48000.0, 440.0, 5.0).passed);
```

Instruments use `.channels(0, 2)`, a `.duration_ms(...)`, and
`.midi(make_note_script(...))` instead of an input buffer. Every generator
documents its exact expression and seed handling, and uses no `std::random_device`
and no clocks, so a render is reproducible across machines.

Sweeping sample rate against block size is a free function rather than a
builder method:

```cpp
auto cells = run_matrix(scenario, kMatrixSampleRates, kMatrixBlockSizes);
```

The canonical grids are `{44100, 48000, 88200, 96000, 192000}` and
`{1, 16, 32, 64, 128, 256, 1024, 4096}`. The full cross product is deliberately
described in-repo as a nightly-scale cost rather than a per-change one, so
callers normally pass subsets. A block size of 1 is in that list on purpose:
it is the cheapest way to catch a `process()` that assumes it will never be
called with a single frame.

Two assertions worth naming. `assert_block_partition_invariant(scenario, {64, 128, 256})`
renders the same input under different block partitions and requires the outputs
to agree — a direct test for state that leaks across buffer boundaries.
`assert_null_near(a, b, tolerance_dbfs = -120.0)` is the null test: it fails if
the **peak** per-sample residual reaches the tolerance, and reports the worst
channel and frame when it does. Peak, not RMS — a single-sample discontinuity
fails it, which for a click is what you want and for a slowly-accumulating
offset is not what you want.

To get from a `Processor` to something the CLI or the quality lab can read, you
render in-process and write a WAV, then point the file-based tools at it.

---

## Two lanes, and why the division is where it is

The audio measurement code sits in two places with a deliberate split.

The **C++ lane** is a library called `pulp-audio-analysis` (in
`tools/audio/analysis/`, namespace `pulp::test::audio`) plus the scenario and
stimulus wiring in `test/support/`. It holds the metrics, the assertions, the
FFT-based frequency response and THD/THD+N analyzers, tone projection, and
alias analysis. It is linked into the shipped CLI, needs no Python environment,
and runs in the ordinary test suite — so this is the lane that can hold a check
that blocks a merge.

The **Python lane** is the quality lab (`tools/audio/quality-lab/`). It holds
the things that are genuinely easier in NumPy: null residual *with* time
alignment (`estimate_global_lag` does an FFT cross-correlation and zeroes its
own confidence when the peak is at a boundary or sits on a periodicity comb;
`local_align` re-aligns per onset), a long-term-average-spectrum log-spectral
distance, spectral flux and centroid, harmonic-to-noise ratio by the
autocorrelation method, Theil-Sen slope for drift, and a Kaiser-sinc resampler
(`resample_to_length`, `resample_by_ratio`) implemented in plain NumPy.

Each of those carries its own limit, and the code states them. The
long-term-average-spectrum distance is computed from magnitude spectra only, so
it is **phase-blind**: a pure all-pass defect, a time reversal, or a delay
leaves the average magnitude spectrum intact and reads as approximately zero
distance. That is a useful property when you want to ignore a latency change
and a disastrous one when the latency change *is* the defect, which is why a
null residual and a spectral distance answer different questions and neither
substitutes for the other. Alignment has a matching honesty: the lag estimator
zeroes its own confidence rather than returning a lag it does not trust, so a
caller that ignores the confidence field gets a number that was never claimed
to be right.

**Why the Python lane cannot hold a blocking check**, concretely and not as a
matter of taste. It is registered in the test runner, but behind a build option
that defaults to off and additionally requires you to hand the build a path to
a pre-provisioned Python interpreter that already has NumPy, `soundfile`, and
the lab itself installed — configuration fails loudly if the option is on and
the path is missing. The continuous-integration images have no such interpreter.
Even with both doors open, the tests carry a label that the blocking lane
explicitly excludes. And the corpus it would measure against is not committed:
the repository holds a manifest and a set of generators, not audio files, with
real material supplied by the developer. So the honest statement is not "it is
not in CI" but "it is reachable from the test runner and is excluded from the
blocking lane on purpose, because it depends on an environment that lane does
not have."

The license guard on the corpus is worth stating precisely, because its name
oversells it. Adding a source records a declared license identifier and checks
it against a hardcoded allowlist of permissive licenses, and records a SHA-256
of the content so a later change is detected. It does not inspect the audio or
verify that the declared license is true. It prevents an accidental commitment,
not a determined one.

### Perceptual scoring

The lab has a layer for full-reference perceptual quality models — ViSQOL,
which reports a MOS-LQO, and two implementations of the ITU-R BS.1387 objective
difference grade. All three are reached only across a process boundary, only
through an explicit environment variable naming the binary (`PULP_VISQOL_BIN`
and siblings). Nothing is bundled, imported, or downloaded; when the variable is
unset each tool independently reports `skipped` with a reason rather than
failing. The public CI never sets those variables, so in practice the whole
layer always skips there.

The layer's own description of what it is for is the right one: a coarse global
guard that says "this got perceptually worse overall" and cannot say "smear at
42 ms". It is advisory and is never a gate. That restraint is deliberate and
worth keeping — a MOS estimate is a model of average listener opinion on the
material the model was fitted to, and turning one into a pass/fail threshold
laundered into a correctness claim is exactly the failure this guide is about.
Speech-intelligibility metrics and no-reference neural speech models are
explicitly out of scope; the lab's contract is reference-versus-candidate over
musical material.

---

## Measurement problems that will bite you

These are the ones that have actually cost time here.

### Deep dynamic range is a window problem, and most windows cannot do it

If you are trying to see a component 100 dB below a loud tone, the analysis
window's side-lobe behaviour decides whether you can see it at all, and the
usual choices cannot. The analyzer exposes six windows, and the header carries
the measured numbers rather than the textbook ones: rectangular at −14 dB first
side lobe, Hann at −31, Hamming at −41, Blackman at −57, flat-top at −93, and
Kaiser tunable by β.

Flat-top is the one that catches people. Its flat-topped main lobe makes a
tone's measured *amplitude* accurate to about 0.01 dB regardless of where it
falls between bins, which is what it is for — but its side lobes are only about
−93 dB and, unlike Hann's or Blackman's, they barely improve with distance. Sixty-four
bins out from a loud tone, flat-top is still at roughly −101 dB where Hann has
fallen to −117. Flat-top buys amplitude accuracy, not dynamic range.

There is a test in the suite that measures exactly this, and the reason it is
worth describing is that it runs a **leakage-only control** for every window: the
same measurement with the quiet tone removed. A 0 dB fundamental at bin 1000.5
and a −100 dBc tone sixteen bins away, at 48 kHz with a 16384-point transform,
reads like this:

| window | with the quiet tone | control (leakage only) | verdict |
|---|---|---|---|
| rectangular | −30.4 dB | −30.4 dB | blind |
| Hamming | −49.4 | −49.4 | blind |
| Hann | −82.3 | −81.5 | blind |
| Blackman | −91.4 | −89.4 | blind |
| flat-top | −96.2 | −93.2 | blind |
| Kaiser β=14 | −100.1 | −135.1 | resolved, 0.1 dB error |

The first five rows report a number that barely moves when the thing being
measured is deleted. That is the whole lesson in one table: each of those
windows returns a confident value, and the value is its own leakage. Only the
last row shows a reading that collapses when the signal is removed, which is
what "this instrument can see it" looks like.

Kaiser at the default β of 14 measures a leakage floor at or below −124 dB at
twelve or more bins from the tone, which is why the suite's stated acceptance
bar is a detection floor of −110 dB or better. β = 12 would measure only about
−102 dB, which is level with a −100 dBc target and therefore useless as a gate.

Kaiser is not a universal answer either. The floor holds in the tone's
neighbourhood, not at the bottom of the spectrum: removing the mean of a
non-integer-cycle tone leaves a DC pedestal that no window touches, and on that
same fixture bins 1 through 3 read about −66, −75, and −92 dB through Kaiser
β=14. A component sitting a few bins from DC is pedestal-limited through any
window.

### Prefer projection to windowing when you can

The way around leakage is not to window better, it is to not use a transform.
`tone_residual_db(samples, cycles_per_sample)` fits a sine and cosine at the
target frequency by least squares — it solves the 2×2 normal equations, so
phase is solved for rather than assumed — subtracts the fitted tone, and returns
the residual energy relative to the fitted energy in dB. Because it subtracts
the tone rather than windowing around it, there is no leakage skirt to fight,
and it works on a tone that is not bin-coherent and a buffer whose length is
not a power of two, neither of which any FFT path here can offer. There is a
test asserting a residual below −100 dB through it.

**What it cannot tell you is what the impurity is.** It lumps harmonics,
aliases, noise, and hum into one number; it answers "how pure is this" and never
"what is wrong with it". For that, a sibling analyzer fits the whole expected
harmonic grid and reports offenders separately, classifying each expected
component as a legitimate harmonic or an alias depending on whether its ideal
frequency was above Nyquist.

### Probing on bin centres flatters a filter

A frequency-sampled design is exact, by construction, at the frequencies it was
sampled at. Probing it only at those frequencies therefore measures the design
grid rather than the filter, and a stopband that is genuinely leaky can read as
deeply attenuated simply because every probe landed where the design was
defined to be right. If the probe frequencies are derived from the same grid
the design used, the measurement cannot fail.

Two habits follow. Place stimulus deliberately off-grid: the spectral-mask test
that proves a muted layout produces silence drives a 997 Hz tone through a
1024-point transform at 48 kHz — a bin width of 46.875 Hz, so the tone sits
about a quarter of the way between bins — and then asserts the output energy is
exactly zero. The deep-dynamic-range test quoted above places its fundamental
at bin 1000**.5** for the same reason. And prefer the projection path when you
can, since it takes an arbitrary frequency rather than a bin index, so
"off-grid" is not a thing you have to arrange.

There is a second, subtler version of this. A metric that counts whether each
band of a filter is *represented* in the transform is counting presence, not
resolution — a band that owns exactly one bin is represented and is also, for
practical purposes, unresolvable. The band-resolution analyzer here reports
that honestly: 32 logarithmically spaced bands across 20 Hz to 20 kHz get only
25 of 32 bands owning a distinct bin at a 1024-point transform, and a narrow
280–340 Hz viewport still gets only 21 of 32 even at 16384 points. Its
`fully_represented()` predicate returns false in both cases. A coverage number
that says "every band is present" and a coverage number that says "every band
is resolved" are different measurements, and conflating them is how a coverage
metric ends up reporting full marks on a band it cannot see.

### Pin the oversampler's filter, or you are measuring the filter

`OversamplerT` offers three kinds — `fir_biquad`, `polyphase_iir`, and
`linear_phase_fir` — and it is a member set with `set_kind()`, defaulting to
**`fir_biquad`**. That default has a worst-case base-band alias rejection of
about 7 dB just above base Nyquist, improving to roughly 9 dB by 0.55 of the
base rate, and a passband about 1.9 dB down at 0.3 of the base rate. Those
figures hold at every oversampling factor.

So any anti-aliasing measurement that does not explicitly select a kind is
measuring a 7 dB filter, whatever the DSP under it does. `linear_phase_fir`
offers a 96 dB design at standard quality and a 140 dB design at pristine, and
the suite holds them to within about 2 dB of those targets. The
below −100 dB residual test mentioned above pins `linear_phase_fir`, `pristine`,
and 16× explicitly — that threshold is a statement about that configuration and
not about the oversampler generally.

### Know which estimator you called

`estimate_frequency()` counts positive-going zero crossings, interpolates the
fractional crossing linearly between bracketing samples, averages the
crossing-to-crossing periods, and reports a confidence of one minus the relative
standard deviation of those periods. It needs at least three crossings and
returns zeros otherwise. It is suited to near-periodic single-pitch material —
test tones, a raw oscillator — and a sawtooth or anything harmonically dense
will defeat it. It is not a pitch tracker, and its own documentation says so.
There is a separate pitch estimator for material that needs one.

### State the detection floor, and prove it with a negative control

Every analyzer has a level below which it cannot distinguish signal from its own
noise, and a threshold set below that floor produces a gate that passes because
the measurement cannot see the failure.

The temptation is to derive the floor analytically — roughly two standard
deviations of the residual. That bound assumes the residual is white, which is
false in exactly the cases that matter, because aliases and distortion products
are discrete lines rather than noise. A residual made of sparse tones has a
standard deviation that says very little about the tallest line in it.

Prove the floor instead. Run the identical measurement on a signal with the
defect removed and show the reading collapse. The window table above is one
instance. Another is a pair of checks in the test suite that exist as a matched
set: one asserts that a resampler matches an independent offline reference to
better than −65 dB, and its sibling injects a known −50 dBFS tone into the
candidate and asserts the residual now reads between −52 and −48 dB. The second
test is what makes the first one mean anything — it demonstrates the instrument
responds to a defect of known size, and does so on the scale it claims to read.

### Perf is tracked, not gated

There is a benchmark differ that compares two JSON results and prints a report,
and its own documentation is explicit that it does not fail the shell when the
current run is worse than the baseline. It is not wired into any workflow or
into the test runner. That is deliberate: timing assertions on shared build
machines track the machine's load rather than the code, which is also why the
blocking lane excludes tests labelled for performance and benchmarking. Read
the benchmark report; do not expect it to stop anything. (The differ's schema
is frame-timing for the UI and GPU path rather than DSP-specific, which is
worth knowing before pointing it at audio.)

### One stored reference, and it is not what its name suggests

There is a file named `test_golden_audio.cpp`, and it is not a golden corpus. It
holds computed expectations — unity gain preserves the signal, +6 dB doubles the
amplitude, bypass passes through — driven through `HeadlessHost` and asserted
arithmetically. It reads no reference file.

The repository does contain exactly one committed audio reference,
`test/fixtures/audio/cross_platform_signal_chain.wav`, and it is a
determinism fixture rather than a quality one: 64 samples through a hardcoded
biquad and a cubic waveshaper, with floating-point contraction disabled, asserted
byte-for-byte so macOS, Linux, and Windows must produce identical IEEE-754
output. It does run on the blocking lane. What does not exist is a corpus of
reference renders that would catch a DSP change sounding worse — there is no
audio ratchet in continuous integration, and the tool that would ratchet one
lives in the advisory Python lane.

---

## Capturing what the UI drew

Rendering a view tree to a PNG needs no window:

```cpp
std::vector<uint8_t> render_to_png(View& root, uint32_t width, uint32_t height,
                                   float scale = 2.0f,
                                   ScreenshotBackend backend = ScreenshotBackend::default_backend);
bool render_to_file(View& root, uint32_t width, uint32_t height,
                    const std::string& output_path, float scale = 2.0f,
                    ScreenshotBackend backend = ScreenshotBackend::default_backend);
```

**The backend argument is load-bearing.** The choices are `default_backend`
(the platform's own raster, which on Apple platforms resolves to
CoreGraphics), `coregraphics`, `skia` (CPU Skia raster), `gpu` (an offscreen
Dawn plus Skia surface), and `auto_select`.

For judging whether an imported design renders correctly, Skia's raster is the
one to compare against, because it is the same rasterizer the live GPU
compositor uses, and CoreGraphics' gradient and anti-aliasing output diverges
from it. Hold that claim at the right scope, though: it is a statement about
matching the compositor, not a ranking of correctness. There was a period when
CoreGraphics rendered conic gradients *more* faithfully than Skia did, because
Skia's sweep shader clamped angles outside its start-to-end window while
CoreGraphics wrapped them. A disagreement between two backends on one primitive
is a claim about that primitive.

For a tree containing anything that needs a GPU host, the `gpu` backend is not
merely preferable, it is the only one that renders the content correctly — the
raster backends will produce an image, and the image will be wrong.

Rather than choosing by hand, there is a dispatcher that inspects the tree:

```cpp
CaptureResult capture_view(View& root, uint32_t width, uint32_t height,
                           float scale = 2.0f,
                           ScreenshotBackend backend = ScreenshotBackend::auto_select);
```

It returns the PNG bytes, a success flag, which backend it actually used, and a
reason string that is empty on success. Three behaviours in it are worth
knowing. A subtree containing a native overlay — a WebView or a platform view
composited by the operating system rather than painted into Pulp's canvas —
returns `ok = false` with a reason, because such overlays are genuinely
invisible to headless capture and the alternative would be silently returning
an image with a hole in it. Anything needing a GPU host routes to the GPU
backend. And every captured frame is checked against a deliberately lenient
"did anything paint at all" floor, so a blank or clear-only result sets
`ok = false` rather than passing as a valid screenshot. The PNG is still
returned when the flag is false, so you can save it and look at why.

For a live window rather than a detached tree there is
`WindowHost::capture_png()`, but be precise about what it returns: it captures
*what the compositor sees*, preferring the operating system's own window
capture and the cached content view before falling back to reading the GPU back
buffer. That ordering makes it faithful to what is on screen and dependent on
the window actually being on screen — it fails on a hidden window. The sibling
`capture_back_buffer_png()` skips straight to the back buffer, which is the
deterministic one to use when you want the pixels the renderer produced rather
than the pixels the desktop composited.

Platform support is uneven and the API says so. macOS has native capture;
Windows and Linux have a built-in Skia raster backend whenever Skia is compiled
in. It is iOS, Android, and non-Apple builds *without* Skia that have no
backend at all, and those need the host application to register one with
`set_screenshot_provider()`. Where a built-in backend does exist, a registered
provider is an override rather than a requirement — it is consulted first and
wins.

Without a backend the functions return an empty buffer rather than throwing,
which is why `has_screenshot_provider()` and `has_screenshot_backend()` exist:
they let a caller distinguish "nothing can capture in this build" from "the
capture ran and failed", two conditions that otherwise produce the identical
empty result. A related trap sits next door — the raw-RGBA sibling of
`render_to_png` is Skia-only and returns an **empty buffer** rather than
failing loudly in a build without it. A suite that skips on an empty result
converts a genuine rasterizer regression into a green run; gate on the build
capability instead.

**What a screenshot cannot see** is worth saying plainly, because a passing
visual comparison is the single most over-trusted result in this repository. An
image proves what was painted into that surface, at that size, at that moment.
It does not prove the control underneath it responds, it does not prove the
value displayed is the value held, and comparing two images from two different
rasterizers will register anti-aliasing and sub-pixel placement as real
differences. A high similarity score between a render and its design source is
also not a statement about layout — the common histogram-style metric is
position-blind, and a design with every element in the wrong place scores the
same as a correct one.

---

## Driving the UI without a window

Widget behaviour is testable in-process with no window at all. Construct the
widget, send it input, assert on its state:

```cpp
TextEditor editor;
editor.on_focus_changed(true);
TextInputEvent te; te.text = "hello";
editor.on_text_input(te);
REQUIRE(editor.text() == "hello");
```

For pointer input there are three simulators on `View`:

```cpp
void simulate_click(Point root_pos);
void simulate_click(Point root_pos, const SimulatedPointer& pointer);
void simulate_drag(Point start, Point end, int steps = 10);
void simulate_drag(Point start, Point end, int steps, const SimulatedPointer& pointer);
void simulate_hover(Point root_pos);
```

`SimulatedPointer` carries a device type, pressure, modifiers, button, and a
pointer id. The id is what makes multi-touch reachable — the gesture arbiter
keys its sessions on it, so a second finger that reuses id 0 replaces the first
rather than pairing with it. Two asymmetries to note before you reach for them:
the pointer overload of `simulate_drag` has no default for `steps`, and
`simulate_hover` has no pointer overload at all.

`KeyEvent` and `TextInputEvent` are deliberately separate types rather than one
event with an optional character. A `KeyEvent` carries a key code, a modifier
mask, and up/down and repeat flags, and no text at all; a `TextInputEvent`
carries a single UTF-8 string and nothing else. Composed input from an input
method arrives only on the second, so a widget that handles keys but not text
input works for a US keyboard and silently drops everything else.

These take a **point**, and that is the important property. `simulate_click`
consults the active overlay slot exactly as the real input path does, then calls
`hit_test(root_pos)` to find the target. It cannot press a control that is
covered, off-screen, or zero-sized, because the hit test will not return it. It
then delivers through the same `deliver_mouse_down` / `deliver_mouse_up` verbs
that the macOS window host and plugin view host call from their own mouse
handlers, so what a test exercises is the path a real click takes.

That last property was once missing, and the gap is instructive. The simulators
originally called only the virtual `on_mouse_*` hooks. A scripted UI — a design
imported and driven from JavaScript — never sees those; it sees the pointer-event
and drag channels. So a script-driven control could be completely dead to input
while every headless test that "clicked" it passed, because the tests were
delivering on a channel the control was not listening to. The tests were real,
the clicks were real, and they were being delivered to the wrong place.

### Activating by selector is a different thing

Contrast that with resolving a control by name and invoking its handler
directly. Pulp has such a path, for designs imported from React and driven
through a JavaScript runtime:
`__pulpActivateMaterializedElement__(selector, eventName, eventData)` looks the
node up in a registry of materialized elements, finds the React callback
registered for that element and event, and **calls it**, synthesizing an event
object rather than delivering one.

Nothing in that path consults painted geometry. There is no occlusion test, no
visibility or `pointer-events` check, no z-order, and no hit test. It will
happily "press" a control sitting behind a modal, underneath an opaque panel,
scrolled out of view, or at zero opacity — a control no human could reach. It
is a legitimate escape hatch and a worthless proof of reachability, and it is
worth knowing that it is not exotic: Pulp's own web-compat shim routes
`Element.prototype.click()` through it, so a scripted UI calling `el.click()` on
itself takes this path. Captured-state replay uses it too.

The same split exists in the lane that drives a real browser for design
capture, and there the two modes sit side by side under different names. A
`click` action probes for a point where the element is genuinely topmost —
centre plus four inset corners, each tested with `document.elementFromPoint`,
requiring the element or a descendant back, and reporting the target as covered
if none qualifies — and only then dispatches a real mouse event at those
coordinates through the browser's input pipeline. A `dispatch-event` action
resolves by `querySelector`, checks only that the bounding rectangle is
non-empty, and dispatches straight at the element.

The distinction is not pedantic. A test suite using the selector path while
believing it tested a click is asserting that a callback fires. It is not
asserting that anybody can fire it.

The native equivalent of doing it properly is `pulp::view::route_context_press(View& root, Point root_pt)`.
It is not a test helper: it is the shared routing verb that the macOS window
host and the plugin view host both call from `rightMouseDown:`. A right-click
has to consult the overlay slot the same way a left-click does, or it opens a
context menu on the control *underneath* an open popover while dismissing the
popover. Calling the same function from a test is what makes the test evidence
about the real gesture.

That distinction has cost real bugs here. Every React `onContextMenu` prop in
every Pulp application was inert for a period, because the JavaScript bridge's
event registration routed `click`, `hover`, `pointer`, `gesture`, and `wheel` to
the native registration path and dropped `contextmenu`. The callback sat in the
registry, the native handler was never armed, and a right-click found nothing.
The test that now covers it deliberately exercises both seams — the exact call
the React prop-applier emits, and the exact function the macOS host reaches from
`rightMouseDown:` — because a test that had stopped at either end would have
passed throughout.

---

## Where the time went

Correctness is one question and cost is another. Pulp can emit a Perfetto trace
— a timeline of where CPU and GPU time went during a run — which you can open in
the Perfetto UI or query with SQL. It is off by default and a default build
links none of it.

Rather than restate it, the reference is [Tracing](tracing.md), which covers
enabling it, the category taxonomy, the real-time-safe fixed-slot path for live
DSP telemetry, and the analysis surface. One discipline from it generalises to
everything else in this guide: a build directory named for tracing proves
nothing about whether tracing is compiled in, and the documented paths verify
the symbols in the binary rather than trusting the directory's name.

---

## How these harnesses fail

Every failure below is a real one from this repository. None of them is a tool
that errored. In each case an instrument returned a confident result while the
defect it existed to catch was present, which is the only failure mode that
matters here, because it is the one that looks like success.

We shipped a click detector that scored the **worst sample-to-sample step** in
the output. It returned an identical number for every condition it was run
against — including both of its controls. What it was actually measuring was
the test tone's own slew rate, which dominated any click in the material, so
the detector could only have fired on a discontinuity *louder than the signal*.
A metric that reports the same value whether or not the defect is present is
not a weak metric; it is not measuring the defect at all. The same gate had a
second problem in the same spirit: it swapped the thing under test exactly once,
so it could not have observed a failure that only appears across a sequence of
swaps.

A coverage metric reported **100%** for a band that measured **18%** within
−3 dB. During tuning it was set aside as "saturated, cannot pick a width" —
which is to say the one instrument positioned to catch the loss being introduced
was discarded for being uninformative, when what it was actually doing was
failing.

A test for a delete confirmation passed because it **injected the exact global
whose absence was the defect**: it set `globalThis.confirm` before pressing the
button. Production never set it. The test supplied what production withheld, so
the code path it exercised was one that could not exist at runtime. The related
shape is a test that encodes the author's mistaken assumption and then confirms
it — one such test asserted that an unhydrated panel renders no controls, which
was precisely the bug.

A lint rule existed, was correct, and had **never once run**. The gate that
would have invoked it ran a curated list of patterns and the rule's pattern was
not in the list. Nothing reported an error, because from the gate's point of
view nothing was wrong: it ran the rules it was asked to run and they passed.

That last one has a mundane cousin worth knowing about, since it applies to
anyone running the test suite here. Asking the test runner for a pattern that
matches nothing prints `No tests were found!!!` **and exits 0**. Verified on a
live build directory: a nonsense filter exits 0 while the same runner reports
10,754 tests with no filter. A typo in a test name is therefore a silent pass,
and a script that selects tests by pattern and checks only the exit code cannot
distinguish "everything passed" from "nothing ran".

### The generalisation

Two rules come out of these, and they are cheap to apply.

**Ask what reading your instrument would give if the defect were maximal.** If
the answer is "the same reading", it cannot see the defect, and no amount of
running it will change that. This is a question you can answer at the point of
writing the measurement, before it has ever produced a number, and it would have
caught the click detector and the coverage metric on the day each was written.
Answering it empirically is better still: make the defect maximal and watch the
number move.

**A control that returns non-zero only proves the tool ran.** It does not prove
the tool ran on the right thing. When the count is knowable, compare it against
what it ought to be — two failures were caught here exactly that way: a
file search returned 56 matches and looked perfectly healthy, when the branch it
should have been reading had 72; and a test selection that found 146 tests
locally found 184 under the same selection in CI. In both cases the tool ran
fine. It ran on the wrong tree.

The corollary is that a negative result deserves *more* scrutiny than a positive
one, not less, because it is the one nobody re-checks. "No aliasing found", "no
files match", "the queue is empty" are all ambiguous by construction: each means
either the thing is absent or you measured the wrong thing, and nothing in the
output distinguishes those two.

### What actually found the worst ones

Worth recording honestly: the three most serious audio defects behind the
stories above were not found by any of this. A band being quieter than drawn was
found by someone playing through the plugin and saying it sounded "kind of like
a vocoder". A convolver discarding history on each swap was found by the same
person saying it sounded "almost like a hard drive seeking", and then "like a
sample is being live-replayed" — the second phrasing named the mechanism, an
error correlating with the output at a short lag rather than with the input. A
control that was not rendering at all was found by someone taking a screenshot
and looking at it.

That is the honest boundary of everything above. The loop described in this
guide removes the cost of *iterating* — it renders, measures, and reports
without anyone present, so a change can be evaluated in seconds and a
regression caught the moment it lands. It does not decide what is worth
measuring. Every instrument here answers a question somebody thought to ask,
and the defects that hurt most were the ones nobody had thought to ask about
yet.

---

## Related

- [Testing](testing.md) — running the suite, writing tests
- [Testing deep-dive](testing-advanced.md) — `HeadlessHost` API, sanitizers, format validators
- [Test lanes](test-lanes.md) — which tests block a merge, and the label taxonomy that routes them
- [Audio Quality Lab](audio-quality-lab.md) — the Python lane in full, including alignment modes
- [Tracing](tracing.md) — Perfetto capture and offline analysis
- [Proving reported latency](latency-proof.md) — a worked example of a self-consistency proof
