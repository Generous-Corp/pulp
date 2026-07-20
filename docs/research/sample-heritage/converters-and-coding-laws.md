# Converters and coding laws

## Evidence

- **Confirmed** — [ITU-T G.711](https://www.itu.int/rec/T-REC-G.711-198811-I/en),
  sections 3.1-3.6, defines A-law and mu-law as two recommended **eight-bit**
  PCM encoding laws, with normative decision, quantized-value, and conversion
  tables. The standard does not define a variable-bit-depth sampler converter.
- **Confirmed** — G.711 associates A-law and mu-law with 13-bit and 14-bit
  uniform-PCM values respectively; its exact wire/code behavior is more than a
  continuous logarithmic transfer curve.
- **Confirmed** — the [PCM56 data sheet](https://www.ti.com/lit/ds/symlink/pcm56.pdf)
  distinguishes 16-bit input resolution, differential linearity, bipolar-zero
  error, THD, dynamic range, settling behavior, and clock timing. Those are
  separate converter properties, not one interchangeable "bit depth" control.
- **Confirmed** — [AES, *Dither in Digital Audio*](https://aes.org/publications/elibrary-page/?id=5173)
  treats rectangular and Gaussian dither as distinct probability distributions
  with different quantizer behavior. A dither level alone does not identify the
  dither process.
- **Confirmed** — TI's [DAC output-response note](https://www.ti.com/content/dam/videos/external-videos/en-us/2/3816841626001/5577272810001.mp4/subassets/TIPL4705-DAC-Output-Response.pdf)
  defines a zero-order hold as retaining an output value for one update period
  and shows its sinc-shaped frequency response.
- **Confirmed** — TI's [sample-and-hold application note](https://www.ti.com/lit/an/snoa223/snoa223.pdf)
  defines hold droop as a rate of output-voltage change caused by hold-capacitor
  leakage; contributing leakage currents need not have the same polarity.

## Mapping to the neutral mechanism

- **Confirmed** — [`SampleHeritageVoiceConverterBlock`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  exposes `linear_pcm`, `mu_law`, and `a_law`, effective quantization from 4 to
  16 bits, a 0-to-1 nonlinearity amount, and 0-to-2 LSB dither.
- **Confirmed** — [`SampleHeritageVoiceDsp`](../../../core/audio/include/pulp/audio/sample_heritage_voice_dsp.hpp)
  applies continuous mu=255 or A=87.6 compression, rounds in that compressed
  domain, expands, and then applies a custom deterministic nonlinearity curve.
  Its seeded dither is bipolar uniform noise, not an exact G.711 octet codec.
- **Probable** — `linear_pcm` is a defensible neutral label for the uniform
  quantizer. Schema-v3 retains the concise `mu_law` and `a_law` family labels,
  but its normative public documentation limits them to the continuous curve
  models above and explicitly disclaims G.711 byte-code compatibility.
- **Test** — `dac_nonlinearity` is a useful fitting control, but its 0-to-1
  amount has no standard physical unit. Promote a value only after static-code,
  low-level-tone, and residual measurements support it.
- **Test** — `hold_samples > 1` and multiplicative `droop` are creative machine
  controls. They are not evidence of a physical DAC's one-update zero-order hold
  or volts-per-second hold droop without a separate calibration model.

## Schema concerns

1. Schema-v3 keeps `mu_law`, `a_law`, and `bit_depth` for concise authoring, but
   public format documentation must retain their narrower continuous-curve and
   effective-resolution definitions.
2. Schema-v3 normatively defines `dither_lsb` as bipolar rectangular dither.
   A future distribution selector would require a schema migration.
3. `dac_nonlinearity` and `droop` are documented as Pulp curve parameters, not
   hardware specifications.

## Unresolved calibration questions

- Is a target compander an exact code table, a continuous transfer curve, or an
  analog companding path?
- Are measured errors best represented by DNL/INL data, a static transfer
  curve, level-dependent residuals, or the existing one-parameter curve?
- What dither PDF, channel relationship, and reset/continuation policy does the
  evidence support?
- Does a hold stage represent one converter update, deliberate repeated samples,
  or a separate track-and-hold circuit with time-based droop?
