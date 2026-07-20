# Neutral Sample Heritage profiles

These schema-v3 profiles are loadable examples for PulpSampler and the
`pulp audio heritage` commands. They demonstrate combinations of the public
mechanisms; they are not measurements or emulations of named commercial
hardware.

- `12-bit-variable-clock.json` combines a reduced machine rate, variable-clock
  pitch, effective 12-bit quantization, hold, reconstruction, and light output
  color. Its authored clock block enables real-time clock control.
- `drop-repeat-grain.json` demonstrates deliberately stepped pitch motion with
  a short zero-order hold and conservative filtering; its authored clock block
  keeps real-time clock control available alongside fixed-clock pitch.
- `cyclic-lengthen.json` demonstrates live cyclic duration change without
  changing the sampler's source traversal or loop policy, with explicit pitch
  and tempo behavior plus an authored real-time clock block.

Use these as starting points, change the `profile_id`, then run:

```sh
pulp audio heritage validate profile.json
pulp audio heritage canonicalize profile.json --out canonical.json
pulp audio heritage inspect canonical.json
```

For evidence-led authoring, see the repository's `heritage-profile` skill and
the Sample Heritage research notes. A profile intended to represent a specific
device needs its own capture provenance and listening validation. Product and
company names remain the property of their respective owners; Pulp is not
affiliated with or endorsed by them.
