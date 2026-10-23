---
breaking = false
summary = "Audio file decoding is bounded by DecodeLimits; header claims are checked before allocation"
---

## What changed
- `FormatRegistry::read(path)` now applies default `DecodeLimits` (2^30 frames, 64 channels, 4 GiB of float output). Every built-in reader (WAV, AIFF, FLAC, MP3, OGG, CoreAudio) checks the header claim before allocating and decodes in bounded chunks; a file whose claim exceeds the limits returns `nullopt` with a logged reason instead of allocating from the header.
- `FormatRegistry::read(path, limits)` takes explicit ceilings; a zero field means no limit, and `DecodeLimits::unbounded()` disables all three. The in-tree callers that read a file the user or host chose (offline rendering, sample assets, impulse responses, memory-mapped fallbacks) pass `unbounded()`.

## Migration steps
- Code that reads untrusted audio (dropped files, downloaded content) can keep the default and treat `nullopt` as a refused file.
- Code that must read arbitrarily long material from a trusted path passes `DecodeLimits::unbounded()` or a larger explicit limit.
- Custom `FormatReader` subclasses may override `read(path, limits)`; the base implementation forwards to `read(path)` without applying limits.
