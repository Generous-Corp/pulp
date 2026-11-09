---
breaking = false
summary = "MemoryMappedAudioReader now scales integer WAV samples by 2^(bits-1), like every other Pulp WAV reader"
---

## Behavior changes
- `MemoryMappedAudioReader` reads WAV through the same decoder as `FormatRegistry::read`, `AudioReader` and `decode_wav`, so its integer samples now match theirs bit for bit. Each integer sample code is divided by 2^(bits-1) instead of 2^(bits-1) - 1. Every value moves toward zero by a factor of (2^(bits-1) - 1) / 2^(bits-1): about 0.8% for 8-bit, about 0.003% (1/32768) for 16-bit, and less than 1.2e-7 relative for 24- and 32-bit. Positive full scale now reads just under 1.0 (32767/32768 for 16-bit), and negative full scale reads exactly -1.0. Float WAV samples do not change.
- The memory-mapped reader also applies the shared WAV acceptance policy. A WAV whose header declares more frames than it holds opens with the frames present, and `info().truncated` is set. A header that contradicts itself is refused.

## Migration steps
- No code change is needed. A test that compares memory-mapped WAV output against hard-coded values scaled by 2^(bits-1) - 1 needs its expectations updated, or a tolerance of one integer step.
