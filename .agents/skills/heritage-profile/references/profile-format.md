# Sample Heritage profile format

Use the checked-out schema and `pulp audio heritage --help` as authoritative.
This reference describes schema version 3 and must be updated when the schema
constant or JSON parser changes.

## Interchange rules

- A profile is data-only JSON with exactly these root fields, in canonical
  order: `schema_version`, `profile_id`, `host_sample_rate`, `voice`, `bus`, and
  `record_commit`.
- Use `schema_version: 3`. Never change a version number to make an old document
  appear current.
- Use a neutral ID beginning with `neutral.`. The remainder accepts lowercase
  ASCII letters, digits, dots, and hyphens without adjacent/trailing separators;
  the complete ID is at most 63 bytes.
- Seeds serialize as decimal strings so all JSON consumers preserve 64-bit
  values. Use `restart_from_profile_seed` unless continuity through serialized
  runtime state is intentional.
- Objects are strict: missing, duplicate, unknown, mistyped, or out-of-range
  data fails closed. Block arrays permit at most eight voice, two bus, and four
  record-commit blocks. Each block type occurs at most once in its domain, and
  an out-of-order block array also fails closed.
- `host_sample_rate` is execution context and does not change profile identity.
  Inspect the profile to obtain its schema and digest; do not invent an identity
  hash by hashing noncanonical input text.

## Typed block palette

Keep only needed blocks. Every object includes its shown `domain`, `type`, and a
boolean `bypass`.

Voice order:

1. `machine_domain`: `sample_rate`
2. `clock`: `ratio`
3. `pitch`: `family` (`variable_clock`, `drop_repeat`, `early_linear`)
4. `converter`: `family` (`linear_pcm`, `mu_law`, `a_law`), `bit_depth`,
   `dac_nonlinearity`, `dither_lsb`, `seed`, `seed_policy`
5. `live_cyclic_stretch`: `factor`, `cycle_ms`, `splice_ms`, `stereo_link`,
   `shuffle_divisions`, `seed`, `seed_policy`
6. `hold_droop`: `mode` (`zero_order`), `hold_samples`, `droop`
7. `reconstruction`: `family` (`one_pole`, `butterworth`, `chebyshev`,
   `elliptic`), `cutoff_law` (`fixed_hz`, `machine_rate_ratio`),
   `cutoff_value`, `order`, `ripple_db`, `stopband_attenuation_db`
8. `analog_color`: `drive`, `asymmetry`, `mix`

Bus order:

1. `noise_idle`: `noise_amplitude`, `idle_amplitude`,
   `tilt_db_per_octave`, `tilt_reference_hz`, `tilt_floor_hz`, `gate`
   (`always_on`, `voice_active`), `seed`, `seed_policy`
2. `output_drive`: `drive`, `ceiling`

Record-commit order:

1. `input_drive_clip`: `drive`, `clip_level`
2. `anti_alias_record_rate`: `filter_family` (`one_pole`, `butterworth`,
   `chebyshev`, `elliptic`), `sample_rate`, `cutoff_law`, `cutoff_value`,
   `order`, `ripple_db`, `stopband_attenuation_db`
3. `converter`: the same converter fields as the voice converter
4. `commit_stretch`: common fields `family` (`cyclic`, `adaptive`), `factor`,
   `zone_start_frame`, and `zone_end_frame`; cyclic adds `cycle_samples` and
   `crossfade_samples`; adaptive adds `decision_hop_samples`,
   `search_radius_samples`, `search_stride_samples`, `crossfade_samples`, and
   `stereo_link`

Do not infer detailed numeric constraints from this summary. Run validation
after every material edit and use the field-path diagnostic to correct a value.
Filter order/ripple/attenuation combinations and rate-relative cutoffs have
cross-field rules that a superficial JSON-schema check cannot prove.

Schema-v3 field semantics are intentionally mechanical:

- `bit_depth` is effective quantizer resolution and may be fractional.
- `mu_law` and `a_law` select continuous mu=255 and A=87.6 companding curves;
  they do not claim G.711 byte-code or table compatibility.
- `dither_lsb` is the amplitude of deterministic bipolar-rectangular dither.
- `dac_nonlinearity` and `droop` are normalized Pulp curve amounts, not DNL/INL
  or volts-per-second measurements.
- `fixed_hz` gives the selected digital filter design edge in hertz;
  `machine_rate_ratio` gives it as a 0-to-0.5 machine-rate ratio. The filter
  family determines the edge convention.
- `idle_amplitude` is always present; `noise_amplitude` follows `gate`. Both are
  normalized linear full-scale amplitudes, not SNR or weighted-noise values.
- `early_linear` is the schema-v3 label for Pulp's two-point linear source
  interpolation. The word `early` is a flavor label, not a historical claim.

## Import, export, versioning, and migration

Import with `pulp audio heritage validate <input> --json`. Export with
`canonicalize`; the canonical file is the portable interchange artifact.
Canonicalize twice and compare bytes, then inspect the re-imported file and
compare its profile digest with the first inspection.

The render command accepts built-in `impulse`, `sine`, and `two-tone` fixtures
or an exact-profile-rate mono WAV. It writes Float32 WAV plus a canonical JSON
report; use optional `--frames` and `--block-size` controls for duration and
partition tests. Active record-commit processing is a separately verified and
reported offline stage rather than implicit cross-rate composition in a live
render.

Keep the original input, canonical export, CLI version, profile schema/digest,
and evidence manifest together. This makes a future migration auditable.

Profiles carry an explicit schema version so future releases can add an
explicit, tested old-to-new migration. Until such a migration is documented by
the installed Pulp release, reject an unsupported version. Never delete the
source profile, silently drop unknown fields, approximate a removed mechanism,
or bump the number by hand. Migrate into a new file, validate and canonicalize
it, rerun analytic/listening controls, and record old/new digests plus the exact
migration command in the artifact manifest.
