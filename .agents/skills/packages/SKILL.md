---
name: packages
description: Search, suggest, add, and browse third-party audio packages. Handles "suggest packages", "add a package", "what packages are available", "search for pitch detection", and package browsing.
requires:
  scripts:
    - tools/packages/registry.json
    - tools/packages/validate_registry.py
---

# Package Manager Skill

Help users discover, evaluate, and add third-party audio libraries to their Pulp projects.

## When to Use

- User asks about available packages or libraries
- User needs a capability not in Pulp's built-in signal subsystem
- User wants to add a third-party library
- User asks about pitch detection, ML inference, resampling, etc.
- User says "suggest packages", "find a library for X", "what's available for Y"

## Package Discovery

Search the registry for packages matching a need:

```bash
./build/tools/cli/pulp search "<query>"
./build/tools/cli/pulp suggest --description "<what the user needs>"
```

Or read the registry directly for full details:

```bash
cat tools/packages/registry.json | python3 -m json.tool
```

## Package Categories

| Category | Packages | What They Provide |
|----------|----------|-------------------|
| dsp | signalsmith-stretch, signalsmith-dsp, cycfi-q, pffft, daisysp | Pitch/time stretch, filters, FFT, pitch detection, physical modeling |
| audio-io | dr-libs, libsamplerate, r8brain-free-src | File decoding, sample rate conversion |
| ml | rtneural | Real-time neural network inference |
| ui | fontaudio | Audio icon font |

## Adding a Package

```bash
./build/tools/cli/pulp add <package-id>
```

This will:
1. Check license compatibility (reject GPL/LGPL)
2. Check platform support against project targets
3. Warn about overlaps with Pulp built-ins
4. Generate `cmake/pulp-packages.cmake` with FetchContent declarations
5. Update `packages.lock.json`, `DEPENDENCIES.md`, `NOTICE.md`
6. Print usage instructions (target_link_libraries + #include)

## Overlap Awareness

Before suggesting a package, check if Pulp's built-in `core/signal/` already covers the need:

- **Filters**: biquad, SVF, TPT, Linkwitz-Riley, ladder, FIR — built-in
- **FFT/STFT**: built-in (use PFFFT only if benchmarks show it's needed)
- **Delay**: built-in
- **Reverb, compressor, oscillator, ADSR**: built-in
- **Pitch detection**: NOT built-in → suggest cycfi-q
- **Pitch/time stretching**: NOT built-in → suggest signalsmith-stretch
- **Neural network inference**: NOT built-in → suggest rtneural
- **Sample rate conversion**: NOT built-in → suggest libsamplerate or r8brain-free-src
- **Physical modeling**: NOT built-in → suggest daisysp

## Browsing Guides

Each package has a detailed integration guide:

```
docs/guides/packages/index.md          — category overview
docs/guides/packages/<package-id>.md   — per-package guide
```

Read these to answer questions about specific packages.

## License Policy

Only MIT/BSD/Apache/ISC/zlib/BSL/public-domain packages are allowed. The registry enforces this. If a user asks about a GPL library, explain the incompatibility and suggest the MIT-licensed alternative from the registry.
