#pragma once

/// @file modular_sequencing.hpp
/// Authored sequence in the signal domain: blocks that remember a pattern, walk
/// it under a clock, and hand back pitch CV and gates.
///
/// `trigger_kit.hpp` ships the *event plumbing* — what an edge is, how a gate
/// is made, how a clock is divided and multiplied. What it does not ship is a
/// pattern. This header is that layer, and it **composes** the kit rather than
/// re-deriving it: every edge in here is a `TriggerDetectT` edge, every slide is
/// a `SlewLimiterT` in constant-time mode, every ratchet is an upstream
/// `ClockMultT`, and every random draw is a seeded `Xorshift32`.
///
/// Three sequencer topologies, chosen because none of them is a linear step
/// sequencer and none of them can be spelled by a MIDI arpeggiator:
///
///   - `StageSeqT` — the **pulse-count stage** sequencer (Ryk M-185 / Intellijel
///     Metropolis lineage): a handful of stages, each holding a pitch, a number
///     of clock pulses it occupies, and a gate mode. Polymeter, held notes and
///     TB-303-lineage slide fall out of it directly.
///   - `CartesianWalkT` — the **2-D grid** sequencer (Make Noise René lineage):
///     a grid of voltages with X and Y clocked *independently*, so two cheap
///     clocks generate a sequence far longer than the grid.
///   - `RunglerT` — the **shift-register/DAC** sequencer (Rob Hordijk's Blippoo
///     Box rungler): deterministic chaos, a long quasi-periodic line out of a
///     few bits of state.
///
/// Plus the two support blocks every patch built from those three needs:
/// `QuantizeScaleT` (snap any CV to a scale on the 1 V/oct standard) and
/// `GateLogicT` / `ProbGateT` (Boolean and probabilistic gate combinators).
///
/// ── Pitch is volts, everywhere ────────────────────────────────────────────
///
/// Pitch is carried as **volts on the 1 V/octave standard** (12 semitones per
/// volt, `units::volts_to_semitones`), not as note numbers, so any block's
/// pitch output feeds any other's pitch input, the quantizer, a slew limiter,
/// or a filter cutoff with no unit negotiation. That interchangeability is the
/// whole reason these are signal blocks rather than note generators.
///
/// ── The two reset verbs (the ambiguity this header closes) ────────────────
///
/// A "reset" input on one hardware module zeroes an LFO's phase, on another
/// zeroes a counter, on another jumps a pattern to step 0, and a user can never
/// predict which. There are exactly **two** verbs here and no third:
///
///   1. **`reset()` — the C++ lifecycle verb.** Returns the object to its
///      as-constructed state: positions 0, counters 0, gates low, held outputs
///      0, **and every RNG rewound to its fixed seed** (series law 2). This is
///      what the framework calls on patch load, transport-stop-to-top and voice
///      steal. It is not clocked by audio. Configuration — stage pitches, grid
///      values, scale masks — survives it; only running state is cleared.
///   2. **`apply_reset_edge()` — the modular reset jack.** A rising edge on a
///      reset input during playback. It sets every **position** (counter, step,
///      grid coordinate) to its start-of-pattern value and forces every **gate**
///      low, but leaves every **continuous output** (pitch CV) holding its last
///      value until the next clock, and **never advances or rewinds an RNG**.
///      Reseeding on a live reset jack would make every reset sound identical,
///      which is musically wrong; reseeding is a `reset()`-only concern.
///
/// `RunglerT` is the single documented exception to the "continuous output
/// holds" half of verb 2: its reset edge restores the seed pattern *and* the
/// DAC output immediately, because re-pinning a wandering rungler line live is
/// the entire point of giving it a reset jack, and its output is a stepped hold
/// that steps by design (§ `RunglerT`) rather than a pitch CV that would click.
///
/// ── Run / stop / reset transport ──────────────────────────────────────────
///
/// The three sequencers take `process(run_high, reset_edge, clock_edge, …)` and
/// obey identical rules:
///
///   1. **Run is a level, not an edge.** While `run_high` is false the block
///      ignores clock edges, holds its position, and forces its gate low — no
///      hung gate across a stop. It does *not* reset position: stop/start is
///      pause/continue, as on hardware.
///   2. **Reset is independent of run.** A reset edge applies verb-2 semantics
///      whether running or stopped; resetting while stopped arms the pattern at
///      the top for the next run.
///   3. **Order of operations inside one `process` call**, fixed and tested:
///      (a) apply the reset edge; (b) if not running, force the gate low and
///      return the held CV; (c) if a clock edge, advance and recompute gate and
///      CV; (d) update the slide smoother; (e) return. Reset and clock in the
///      same sample is therefore deterministic — reset wins, then the clock
///      advances from the top, so the downbeat fires on that very clock.
///      Because (b) returns before (d), a slide freezes where it was when the
///      transport stopped and resumes from there, which is what "position held"
///      has to mean for a continuous output.
///   4. **No free-running.** Nothing here generates its own clock. Timing comes
///      from `ClockDividerT` / `ClockMultT` upstream so there is one timing
///      authority in a patch.
///
/// `TransportEdgeT` is the only place reset-edge detection lives, so every
/// sequencer agrees on what an edge is.
///
/// ── Oversampling policy ───────────────────────────────────────────────────
///
/// **None, deliberately.** Every output here is a stepped hold — a DAC level, a
/// quantized semitone, a gate — and the steps *are* the sound, not an artefact
/// of undersampling a continuous nonlinearity. There is no gain-carrying
/// nonlinearity in this header to alias, so band-limiting would only remove the
/// intended edge. A caller who wants the steps softened puts a `SlewLimiterT`
/// downstream, which is a musical choice and not ours to make.
///
/// ── Determinism ───────────────────────────────────────────────────────────
///
/// Series law 2: every draw is a seeded `Xorshift32` rewound by `reset()`;
/// seeds are compile-time/config values and never automatable parameters. Every
/// block is per-sample and carries no block-length state, so a render is
/// bit-identical across buffer sizes as well as across resets.
///
/// RT contract: `prepare()` recomputes sample counts and allocates nothing.
/// `set_*`, `process()`, `apply_reset_edge()` and `reset()` allocate nothing,
/// take no locks, and perform no I/O. All state is POD and fixed-capacity; a
/// default-constructed instance is already in the state `reset()` would put it
/// in. Every block reports `latency_samples() == 0`: a slide is a continuous
/// smoother, not a delay.

#include <pulp/signal/transport_edge.hpp>
#include <pulp/signal/stage_sequencer.hpp>
#include <pulp/signal/cartesian_walk.hpp>
#include <pulp/signal/rungler.hpp>
#include <pulp/signal/scale_quantizer.hpp>
#include <pulp/signal/gate_logic.hpp>
#include <pulp/signal/probability_gate.hpp>
