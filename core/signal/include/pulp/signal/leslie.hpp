#pragma once

/// @file leslie.hpp
/// The two mechanical pitch-modulators of the electric-organ world: a rotary
/// speaker cabinet and the Hammond console's internal scanner vibrato.
///
/// They wear similar clothes and are different physics, which is why they are
/// one family umbrella and two focused class headers rather than one class with
/// a mode switch:
///
/// - **`LeslieRotaryT`** is a real horn and a real bass baffle physically
///   SPINNING inside a wooden box. The rotation produces a genuine Doppler
///   pitch shift (the source moves toward and away from the mic), a tremolo
///   (the horn's beam sweeps past the mic), and a timbral sweep (a horn is
///   brighter on-axis). Two rotors at different rates and different inertias,
///   split by a crossover, recorded by a stereo pair, inside a reflective
///   cabinet.
/// - **`ScannerVibratoT`** has no moving air at all. A passive lumped LC delay
///   line is tapped along its length and a motor-driven rotating capacitor
///   sweeps a pickup across the taps, so the output delay ramps up and back
///   each revolution. A time-varying delay is a pitch shift. Mix the swept
///   output back with the dry organ and the comb interference is the Hammond
///   chorus — a flanger built from a transmission line and a commutator in
///   1949.
///
/// ## Why a Leslie never sounds like an LFO
///
/// Nothing in it is one clean sine, and each of those is a separate structural
/// fact rather than a voicing choice:
///
/// - **The two rotors run at deliberately different rates**, so their
///   modulations beat at the difference frequency — 1.0 Hz at the tremolo
///   defaults, 0.20 Hz at chorale. Locking them equal would remove the single
///   most identifying feature of the sound.
/// - **The two rotors have different INERTIA.** The horn is light and arrives
///   at a new speed in about a second; the drum is heavy and takes several, and
///   coasts down slower than it spins up. A speed change is therefore not a
///   ramp, it is two ramps of different lengths — the horn gets there first and
///   the drum smears in behind it. That asymmetry IS the gesture; see
///   `set_speed`.
/// - **One cosine drives three effects.** The facing angle sets the path length
///   (Doppler), the beam gain (tremolo) and the shelf gain (brightness), so
///   they are inseparably locked in the model exactly as they are in the
///   cabinet. Three independent LFOs would let them drift apart, and the sound
///   would come apart with them.
/// - **Two mics at different angles** see the horn face them at different
///   phases, which is where the ping-pong swirl comes from — not from a stereo
///   widener downstream.
///
/// ## Series law 1 does not apply here
///
/// Stated explicitly rather than omitted: **neither engine contains a feedback
/// path.** Both are strictly feedforward — modulated delay, parallel reflection
/// taps, an amplitude multiply, and a dry mix. There is no loop for a
/// small-signal gain to accumulate around, so there is nothing to
/// unity-compensate or bound as a loop gain. What the registry needs instead is
/// a CONSTRUCTIVE-SUM bound: the worst case is horn + drum + reflections + dry
/// all peaking together. That bound is measured by the suite's parameter sweep,
/// not estimated — see `kWorstCaseGain` on each class.
///
/// ## Oversampling: none, and that is a policy, not an omission
///
/// Series law 4 asks every module to state its policy. Neither engine contains
/// an aliasing nonlinearity: the amplitude and directivity multiplies are by
/// band-limited control signals, and the crossover, shelves and delay reads are
/// linear. There is no in-band alias source to suppress, so oversampling would
/// cost cycles and change nothing. The Leslie tube amp's growl IS a
/// nonlinearity and is deliberately out of scope — compose a saturator ahead of
/// this node, where it can carry its own oversampling policy.
///
/// ## RT contract
///
/// `prepare()` sizes the delay lines and may allocate. `set_*`, `process()`,
/// `process_block()` and `reset()` never allocate, never lock, and never
/// perform I/O. All buffers are sized in `prepare()` from the MAXIMUM of every
/// parameter's range, so no later `set_*` can ask for a read the buffer cannot
/// serve. State is POD; zero-init is a valid stopped cabinet. The only
/// randomness is the optional seeded mechanical drift, rewound by `reset()` and
/// never automatable (series law 2).
///
/// References: Smith, Serafin, Abel & Berners, "Doppler Simulation and the
/// Leslie", Proc. DAFx-02, Hamburg, 2002 — the Doppler-via-modulated-delay
/// method, the rotating-source geometry, the two-delay-line stereo mic model,
/// the angle-dependent directivity filtering, and the first-reflection filters.
/// The speed of sound is the standard dry-air figure from the ideal-gas
/// approximation `c ≈ 331.3 + 0.606·T(°C)` m/s at 20 °C. Rotor speeds, inertia
/// times, the crossover corner, the scanner rate and the line-box tap topology
/// are documented hardware BEHAVIOUR implemented as named design parameters —
/// see the honest-gap notes on each constant; no service-manual numbers are
/// reproduced.

#include <pulp/signal/leslie_rotary.hpp>
#include <pulp/signal/scanner_vibrato.hpp>
