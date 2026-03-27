# CHOC Dependency Audit

Date: 2026-03-24

## Status
CHOC is already in DEPENDENCIES.md (ISC license) and fetched via CMake FetchContent.
Currently **not used in any source code** — only declared as a dependency.

## High-Value Modules for Pulp

### Phase 6 (GPU Rendering / UI) — Critical
| Module | Use | Priority |
|--------|-----|----------|
| `choc::javascript::Context` | JS engine abstraction (QuickJS/V8/JSC) | **CRITICAL** — use instead of building our own |
| `choc::file::FileWatcher` | Hot-reload for JS UI files | HIGH |
| Lock-free FIFOs | Audio thread → UI thread data transfer | HIGH |
| `choc::json` | Design token parsing, config | MEDIUM |
| WebView | Optional embedded web content panels | LOW |

### Phase 9 (DSP / Examples) — Useful
| Module | Use | Priority |
|--------|-----|----------|
| Audio file I/O (WAV/FLAC/OGG/MP3) | File reading for PulpSampler | MEDIUM |
| MIDI file I/O | MIDI sequence import/export | LOW |
| Sample buffers | Interleaved/planar conversion | LOW |

### General Utilities — Nice to Have
| Module | Use | Priority |
|--------|-----|----------|
| String utilities | Complements std::string | LOW |
| Base64, xxHash | State encoding, hashing | LOW |
| HTTP server | Possible MCP transport | LOW |
| Unit test framework | We use Catch2 instead | SKIP |
| Platform detection | We have our own | SKIP |

## Recommendation
**Use CHOC's JavaScript engine abstraction for Phase 6.** This is the single highest-value module — it provides exactly the `QuickJS/V8/JSC` adaptive engine we planned, already battle-tested by Tracktion and iPlug3. Building our own would take weeks.

Also use: file watcher (hot-reload), lock-free FIFOs (audio→UI), JSON parser (tokens).
