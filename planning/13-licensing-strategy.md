# Licensing and Third-Party Strategy

## Overview

This document defines Pulp's licensing model, inventories all third-party dependencies, analyzes license compatibility, and establishes governance rules for contributions and dependency management. The overarching principle is: Pulp and its dependency chain must remain permissively licensed with no copyleft obligations.

---

## Pulp License

### Recommendation: MIT License

Pulp itself is released under the **MIT License**.

### Rationale

| Factor | MIT | Apache 2.0 | AGPL/Commercial Dual |
|--------|-----|------------|---------------------|
| Adoption friction | Minimal | Low | High |
| Corporate legal approval | Easy | Easy | Difficult |
| Patent grant | No | Yes | Varies |
| Copyleft | No | No | Yes (AGPL) |
| Competitive positioning | Strongest | Strong | Weakest (for adoption) |

The MIT license provides:

1. **Maximum adoption:** No legal barriers for any developer or company. Corporate legal teams routinely approve MIT dependencies without review.
2. **Competitive advantage:** The audited framework uses an AGPL/commercial dual-license model that requires either open-sourcing derivative works or purchasing a commercial license. Pulp's MIT license removes this barrier entirely.
3. **Simplicity:** The MIT license is short, well-understood, and unambiguous. There are no special conditions, patent clauses, or attribution requirements beyond preserving the copyright notice.
4. **Ecosystem compatibility:** MIT is compatible with virtually every other open-source license, allowing Pulp to be used in projects of any license type.

### Alternative: Apache 2.0

If patent protection is valued, Apache 2.0 is the recommended alternative:

- Includes an explicit patent grant from contributors
- Includes a patent retaliation clause (contributors who sue over patents lose their license grant)
- Slightly longer and more complex than MIT
- Equally permissive for practical purposes

### Decision

Use **MIT** for simplicity and maximum adoption. If the project receives contributions from organizations with significant patent portfolios and patent protection becomes a concern, consider switching to Apache 2.0 before v1.0 release.

---

## Third-Party Dependency Inventory

### Core Dependencies (Bundled or Fetched)

| Component | License | Can Bundle? | Integration Method | Size Impact | Notes |
|-----------|---------|-------------|-------------------|-------------|-------|
| VST3 SDK | MIT | Yes | Submodule or FetchContent | Medium | Steinberg's official SDK. MIT-licensed since 2023. Freely redistributable. Headers and validator included. |
| AudioUnit SDK | Apache 2.0 | Yes | System headers (macOS) | None | Apple's open-source Audio Unit SDK. Available via Xcode and as open-source headers. |
| CLAP | MIT | Yes | Header-only | Minimal | Single-header plugin API. Extremely lightweight. MIT-licensed. |
| LV2 SDK | ISC | Yes | Header-only | Minimal | ISC license (functionally equivalent to MIT). Header-only integration. |
| Oboe | Apache 2.0 | Yes | Submodule or FetchContent | Small | Google's Android audio library. Only needed for Android target. |

### Optional Dependencies

| Component | License | Can Bundle? | Integration Method | Notes |
|-----------|---------|-------------|-------------------|-------|
| Visage | TBD (verify) | Verify before bundling | Submodule via /pulp:setup-gpu | GPU UI framework. License must be verified as permissive before integration. If not permissive, use as external dependency that developers obtain independently. |
| bgfx | BSD 2-Clause | Yes | Via Visage submodule | GPU abstraction layer. Pulled in transitively through Visage. |
| Skia | BSD 3-Clause | Yes | FetchContent or pre-built | 2D rendering backend for Linux and as CPU fallback. Large binary but permissively licensed. |

### Utility Dependencies

| Component | License | Can Bundle? | Notes |
|-----------|---------|-------------|-------|
| zlib | zlib License | Yes | Compression for state serialization, resource bundling. Extremely permissive. |
| HarfBuzz | MIT (Old MIT) | Yes | Text shaping for internationalized UI text. Required for proper glyph positioning. |
| FreeType | FreeType License (BSD-like) | Yes | Font rendering. Primarily needed on Linux where system text rendering APIs are limited. On macOS/Windows, prefer platform-native text rendering. |
| SheenBidi | Apache 2.0 | Yes | Bidirectional text support. Required for proper RTL language rendering in UI. |
| Catch2 | BSL-1.0 (Boost) | Yes | Test framework. Dev dependency only, not included in release builds. |
| nlohmann/json | MIT | Yes | JSON parsing for configuration files, appcast parsing. Header-only. |

### Proprietary Dependencies (NOT Bundled)

| Component | License | Can Bundle? | Acquisition Method |
|-----------|---------|-------------|-------------------|
| AAX SDK | Proprietary (Avid) | No | Developers must obtain directly from Avid via developer program. Pulp provides the adapter code; developers supply the SDK. |
| ASIO SDK | Proprietary (Steinberg) | No | See ASIO Considerations section below. |

---

## License Compatibility Matrix

All permissive licenses in Pulp's dependency chain are compatible with MIT:

```
MIT (Pulp)
  +-- MIT (VST3 SDK)           Compatible
  +-- Apache 2.0 (AU SDK)      Compatible
  +-- MIT (CLAP)               Compatible
  +-- ISC (LV2)                Compatible (ISC ~ MIT)
  +-- Apache 2.0 (Oboe)        Compatible
  +-- BSD 2-Clause (bgfx)      Compatible
  +-- BSD 3-Clause (Skia)      Compatible
  +-- zlib (zlib)              Compatible
  +-- MIT (HarfBuzz)           Compatible
  +-- FreeType License          Compatible (BSD-like)
  +-- Apache 2.0 (SheenBidi)   Compatible
  +-- BSL-1.0 (Catch2)         Compatible (test only)
  +-- MIT (nlohmann/json)      Compatible
```

**No compatibility issues exist** among the listed dependencies. All are permissive licenses that allow:
- Commercial use
- Modification
- Distribution
- Private use

The only obligations are:
- Preserve copyright notices (all permissive licenses)
- Include license text in distributions (all permissive licenses)
- State changes if modified (Apache 2.0 only, for the modified files)

### Problematic Dependencies

The **AAX SDK** (proprietary, Avid) cannot be bundled or redistributed. Developers who need Pro Tools support must:
1. Join the Avid Developer Program
2. Accept Avid's SDK license agreement
3. Download the AAX SDK independently
4. Place it in `external/aax/` (which is gitignored)
5. Enable AAX format in their CMake configuration

Pulp's AAX adapter code (which wraps Pulp's plugin interface to the AAX API) is MIT-licensed and included in the repository. Only the AAX SDK headers and libraries are proprietary.

---

## Attribution Requirements

### NOTICE File

The repository root contains a `NOTICE` file listing all bundled third-party dependencies with their licenses:

```
Pulp Audio Framework
Copyright (c) [year] [copyright holder]

This product includes software developed by third parties:

---

VST3 SDK
Copyright (c) Steinberg Media Technologies GmbH
Licensed under the MIT License
See external/vst3sdk/LICENSE.txt

---

CLAP (CLever Audio Plugin)
Copyright (c) Alexandre Music & CLAP contributors
Licensed under the MIT License
See external/clap/LICENSE

---

[... additional entries ...]
```

### Per-Dependency License Files

Each dependency in `external/` retains its original `LICENSE` (or `COPYING`) file:

```
external/
  vst3sdk/LICENSE.txt
  clap/LICENSE
  lv2/COPYING
  oboe/LICENSE
  bgfx/LICENSE
  skia/LICENSE
  zlib/LICENSE
  harfbuzz/COPYING
  freetype/docs/FTL.TXT
  sheenbidi/LICENSE
  catch2/LICENSE.txt
  json/LICENSE.MIT
```

### Built Binary Attribution

Distributed binaries include attribution in one or more of:
- **About dialog** in standalone applications and plugin UI
- **Documentation** included with the installer
- **NOTICE file** bundled in the installation directory
- **Plugin metadata** (where format supports it, e.g., VST3 info.plist)

---

## Governance

### Contribution License

All contributions to Pulp are made under the MIT License. Contributors agree to this via one of:

**Option A: Developer Certificate of Origin (DCO)**
- Contributors add `Signed-off-by: Name <email>` to commit messages
- This certifies the contribution is their original work (or they have the right to submit it)
- Enforced via a CI check on pull requests
- Lower friction than a CLA

**Option B: Contributor License Agreement (CLA)**
- Formal agreement granting the project the right to use the contribution
- Provides stronger legal protection for the project
- Higher friction (one-time signature required)

**Recommendation:** Use DCO (Option A) for lower contributor friction. Switch to CLA only if the project is adopted by a foundation or corporate entity that requires it.

### Dependency Acceptance Criteria

Before any new dependency is added to Pulp, it must pass the following checks:

1. **License must be permissive:** MIT, Apache 2.0, BSD (2-Clause or 3-Clause), ISC, zlib, BSL-1.0, or similarly permissive
2. **No copyleft in core:** GPL, LGPL, AGPL, MPL, or other copyleft licenses are NOT permitted in any core subsystem
3. **Copyleft only in optional modules:** If a copyleft dependency is ever needed, it must be in a separately-linked optional module that can be excluded from builds
4. **License file must exist:** The dependency must have a clear, machine-readable license file
5. **Transitive dependencies checked:** All transitive dependencies must also meet these criteria
6. **NOTICE file updated:** Adding a dependency requires updating the NOTICE file
7. **Size justification:** Large dependencies (>1MB source) require justification for their inclusion

### License Audit Process

Run periodically (and before each release):

1. Enumerate all dependencies (direct and transitive)
2. Verify each dependency's license is on the approved list
3. Verify license files are present in `external/`
4. Verify NOTICE file is up to date
5. Flag any changes since last audit

This can be automated via tools like `license_finder`, `licensee`, or a custom script.

---

## ASIO Considerations

The Steinberg ASIO SDK has a restrictive license that prohibits redistribution and has specific usage conditions. This is a consideration for Windows low-latency audio.

### Options

**Option 1: Do not support ASIO directly**
- Push users toward WASAPI in low-latency (exclusive) mode
- WASAPI exclusive mode provides comparable latency to ASIO on modern Windows
- Simplest approach, no licensing complications
- Some professional users and legacy hardware require ASIO

**Option 2: Let developers obtain ASIO SDK independently**
- Same model as AAX: developers download the SDK themselves
- Pulp provides ASIO adapter code (MIT-licensed)
- SDK is gitignored and not distributed
- Developers who need ASIO enable it in their build configuration

**Option 3: Use PortAudio as intermediary**
- PortAudio (MIT-licensed) has an optional ASIO host API
- If the developer has the ASIO SDK, PortAudio can use it
- If not, PortAudio falls back to WASAPI/DirectSound
- Adds PortAudio as a dependency but decouples Pulp from ASIO licensing

### Recommendation

**Option 2** (independent acquisition) for maximum flexibility, with **Option 3** (PortAudio) as a secondary consideration if the PortAudio dependency is otherwise justified.

Rationale:
- Option 2 is consistent with the AAX SDK approach (developers are already familiar with obtaining proprietary SDKs independently)
- WASAPI low-latency mode is sufficient for most users
- ASIO is primarily needed for professional audio interfaces with vendor-specific ASIO drivers

---

## What NOT to Include

To maintain clean-room integrity and avoid any licensing contamination:

1. **No source code from the audited framework** -- not a single line of source, header, or implementation
2. **No API names from the audited framework** -- Pulp defines its own class names, method names, and module names
3. **No project file generators from the audited framework** -- Pulp uses standard CMake, not proprietary project generators
4. **No module format from the audited framework** -- Pulp uses standard CMake modules, not proprietary module definitions
5. **No irrelevant bundled libraries** -- only include dependencies that serve Pulp's actual needs (e.g., no physics engines, no web browsers, no XML parsers beyond what is needed)
6. **No "inspired by" reimplementations** -- Pulp's API is designed from first principles and specification documents, not by studying and replicating another framework's API

### Verification

Before public release, perform a final contamination review:
- Search all source files for any identifiers, comments, or patterns that reference the audited framework
- Verify no source files were copied or adapted from the audited framework
- Verify API surface is independently designed (different naming, different architecture patterns)
- Document the clean-room process used for each subsystem
