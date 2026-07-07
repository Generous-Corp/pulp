# Scala SCL/KBM Tuning

Pulp can load local Scala `.scl` scales and `.kbm` keyboard mappings through
the optional Surge Synth Team `tuning-library` integration. This is the direct
file-loading path for products that want to ship or import tuning files without
requiring a session-wide MTS-ESP master.

## Add It To A Pulp Project

```bash
pulp add sst-tuning-library
```

That fetches Surge Synth Team's header-only tuning library source and creates a
local `sst::tuning-library` interface target without adding the upstream
command-line tools or tests to your build.

If you are building Pulp itself and want the provider available through
`pulp::midi`, configure with:

```bash
cmake -S . -B build -DPULP_ENABLE_SCALA_TUNING=ON
```

If your project consumes an installed Pulp SDK, keep the tuning library
project-local and attach Pulp's wrapper to your plugin target:

```cmake
include(cmake/pulp-packages.cmake OPTIONAL)

pulp_add_plugin(MySynth
    FORMATS CLAP Standalone
    CATEGORY Instrument
    ACCEPTS_MIDI
    SOURCES src/PluginProcessor.cpp)

pulp_enable_midi_tuning_provider(MySynth SCALA)
```

The helper uses the SDK's built-in provider when available. If the SDK was built
without Scala tuning support, it compiles Pulp's small wrapper source into
`MySynth` and links the `sst::tuning-library` target from
`pulp add sst-tuning-library`.

Then use the provider-neutral tuning API:

```cpp
#include <pulp/midi/scala_tuning.hpp>

pulp::midi::ScalaTuningProvider tuning;
std::string error;
if (!tuning.load_scl_kbm_files("scales/porcupine.scl", "scales/keyboard.kbm", &error)) {
    // Report the file/parse error on the UI or main thread.
}

auto note = tuning.note_to_frequency(60, midi_channel);
if (note.valid && !note.should_filter_note) {
    voice.set_frequency(note.frequency_hz);
}
```

File loading and parsing belong on the UI/main side. Voice code should depend on
`pulp::midi::TuningProvider` and query frequencies in the same place it already
handles note-on pitch.

## With MTS-ESP Mini

ODDSound's free MTS-ESP Mini can load `.scl`, `.kbm`, `.tun`, and MTS SysEx, then
publish the active tuning to all MTS-ESP-aware plugins in the DAW session. Use
Pulp's `MtsEspTuningProvider` when that session-wide behavior is the product
model.

For products that should support both direct local files and a session-wide
MTS-ESP master, compose the two providers:

```cpp
#include <pulp/midi/mts_esp_tuning.hpp>
#include <pulp/midi/scala_tuning.hpp>

auto local = std::make_unique<pulp::midi::ScalaTuningProvider>();
local->load_scl_file("factory/default.scl");

pulp::midi::MtsEspFallbackTuningProvider tuning(std::move(local));
auto note = tuning.note_to_frequency(midi_note, midi_channel);
```

`MtsEspFallbackTuningProvider` uses an active MTS-ESP master or parsed MTS SysEx
when present. If no MTS session tuning is active, it falls back to the local
provider, which can be a Scala tuning provider or any other `TuningProvider`.

## Importing Existing Projects

Importers should map existing Scala/SCL/KBM file loading to
`ScalaTuningProvider`, copy detected `.scl` / `.kbm` assets into the emitted
scaffold as source assets, and keep file I/O off the audio callback. The
ProjectIR `integration_requirements` section carries the required
`sst-tuning-library` package, the `PULP_ENABLE_SCALA_TUNING` provider option,
and the copied tuning assets. The shared emitter turns those requirements into
the `pulp_enable_midi_tuning_provider(... SCALA)` CMake helper call so one
actionable setup path works for both source-built Pulp and installed-SDK
projects.

If the source project also used ODDSound's `libMTSClient.h`, enable both
optional integrations and preserve the original priority policy:

- MTS-ESP session/master tuning wins when the product was designed to follow a
  DAW-wide tuning source.
- Local `.scl` / `.kbm` tuning wins when the product treated tuning files as
  project or preset state.
- If both are supported, use `MtsEspFallbackTuningProvider` so MTS can override
  local file tuning only while an MTS session source is active.

`.tun` files are not parsed directly by this provider. Importers should copy
detected `.tun` assets into the scaffold, mark them for manual review, and
direct the port to MTS-ESP Mini session tuning or a conversion to `.scl` /
`.kbm` before loading it into `ScalaTuningProvider`.

## License

Surge Synth Team `tuning-library` is MIT licensed. It is fetched only when
explicitly enabled with `PULP_ENABLE_SCALA_TUNING=ON` or
`pulp add sst-tuning-library`, so it is not part of Pulp's default dependency
chain. Installed SDKs ship only the Pulp-owned wrapper source needed by
`pulp_enable_midi_tuning_provider()`, not Surge's tuning-library source itself.
