---
breaking = false
summary = "interpret_wav_format's fact-count overload and WavFormatFacts::total_frames are deprecated"
---

## Deprecations
- The three-argument `interpret_wav_format(fmt_payload, data_size,
  fact_sample_count)` is `[[deprecated]]`. The fact chunk's sample count never
  changed the result; call the two-argument form.
- `WavFormatFacts::total_frames` is deprecated in its documentation. It still
  holds the whole frames of the `data_size` passed to `interpret_wav_format`.
  For a file's frame counts read `AudioFileInfo` (`num_frames`,
  `declared_frames`, `truncated`) or `WaveDecision` from `decide_wave`, which
  also account for bytes the file does not hold.

## Migration steps
- Drop the third argument from any `interpret_wav_format` call. A caller that
  must refuse a truncated WAV sets `WavDecodeLimits::refuse_truncated` or
  checks an inspection with `wav_decode_admits`.
