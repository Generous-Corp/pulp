# ara-pitch-tracker — minimum viable ARA-aware CLAP plug-in

Smallest possible end-to-end integration of ARA over CLAP. Demonstrates:

1. Processor override — `PitchTracker::create_ara_document_controller()`
   returns a concrete `AraDocumentController` subclass.
2. CLAP adapter — `clap_plugin::get_extension(kClapAraFactoryExtension)`
   automatically surfaces the Pulp ARA factory (published by
   `core/format/src/ara_factory.cpp`). No extra adapter code needed here.
3. Controller lifecycle — `begin_editing` / `end_editing` /
   `notify_audio_source_content_changed` log so developers can verify
   the handshake from an ARA-aware host.

Real pitch analysis (YIN, spectral peak, autocorrelation) is left as an
exercise. The point is the host handshake, not the DSP.

**Workstream:** 06 slice A3.

## Build

```bash
cmake -S . -B build -DPULP_ENABLE_ARA=ON -DPULP_ARA_SDK_DIR=$PWD/external/ara-sdk
cmake --build build --target AraPitchTracker-CLAP
```

Without `PULP_ENABLE_ARA=ON` the plug-in still builds and runs as a
normal CLAP effect; it just won't advertise the ARA extension.

## Validate

Load in a CLAP-ARA host (Bitwig with ARA extension, REAPER, Studio
One 7 with its CLAP-ARA bridge) and open the plug-in. Watch the log:

```
ara-pitch-tracker: begin_editing
ara-pitch-tracker: audio source content changed ...
ara-pitch-tracker: end_editing
```

If nothing logs, the host isn't calling the ARA factory. Check the
host supports ARA over CLAP (a few are still VST3/AU-only for ARA).

## See also

- `.agents/skills/ara/SKILL.md` — per-adapter usage recipes
- `docs/guides/host-matrix.md` — validated host × format combinations
- Issue #219 — ARA workstream tracking
