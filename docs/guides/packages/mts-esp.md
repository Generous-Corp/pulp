# MTS-ESP

ODDSound MTS-ESP is an optional microtuning client integration. It lets a
Pulp synth query an MTS-ESP master when one is installed in the DAW session,
and it falls back to local 12-TET or incoming MIDI Tuning Standard SysEx data
when no master is present.

## Add It To A Pulp Project

```bash
pulp add mts-esp
```

That fetches ODDSound's client source and creates a local `mts_esp_client`
CMake target. Link it from your plugin target if you are using MTS-ESP outside
Pulp's built-in MIDI module:

```cmake
target_link_libraries(MyPlugin PRIVATE mts_esp_client)
```

If you are building Pulp itself and want the provider available through
`pulp::midi`, configure with:

```bash
cmake -S . -B build -DPULP_ENABLE_MTS_ESP=ON
```

Then use the provider-neutral tuning API:

```cpp
#include <pulp/midi/mts_esp_tuning.hpp>

pulp::midi::MtsEspTuningProvider tuning;
auto note = tuning.note_to_frequency(60, midi_channel);
if (note.valid && !note.should_filter_note) {
    voice.set_frequency(note.frequency_hz);
}
```

## Provider-Neutral Tuning

`MtsEspTuningProvider` is only one implementation of Pulp's
`pulp::midi::TuningProvider` interface. Plugin code should depend on
`TuningProvider` where possible and choose the concrete provider at setup time:

```cpp
void Voice::start_note(int note, int channel, const pulp::midi::TuningProvider& tuning) {
    auto tuned = tuning.note_to_frequency(note, channel);
    if (!tuned.valid || tuned.should_filter_note) {
        active = false;
        return;
    }

    set_frequency(tuned.frequency_hz);
}
```

That keeps MTS-ESP optional and leaves room for first-party Pulp tuning later.
The optional `ScalaTuningProvider` already implements the same interface for
direct `.scl` / `.kbm` files, and future tuning-table objects, host-provided
tuning bridges, or project-local microtuning engines should do the same.
Existing processors then switch providers without changing their note/voice code.

## With Local SCL/KBM Files

ODDSound's free MTS-ESP Mini can load `.scl`, `.kbm`, `.tun`, and MTS SysEx and
publish that tuning session-wide. That is often the right UX when a producer
wants every MTS-aware plugin in the DAW to follow the same tuning.

When a product needs project-local file tuning too, enable both optional
providers and wrap the local file provider with `MtsEspFallbackTuningProvider`:

```cpp
auto local = std::make_unique<pulp::midi::ScalaTuningProvider>();
local->load_scl_kbm_files("preset.scl", "preset.kbm");

pulp::midi::MtsEspFallbackTuningProvider tuning(std::move(local));
```

The wrapper uses an active MTS-ESP master or parsed MTS SysEx when present. If
no MTS session tuning is active, it falls back to the local provider.

## Porting Existing MTS-ESP Calls

JUCE and iPlug2 do not ship MTS-ESP as a framework feature, but many projects
include ODDSound's `libMTSClient.h` directly. Importers should detect that
include, `MTS_*` calls, or bundled `Client/libMTSClient.cpp`, declare the
`mts-esp` package plus `PULP_ENABLE_MTS_ESP` in ProjectIR
`integration_requirements`, and rewrite the call sites toward
`TuningProvider`.

| Existing client call | Pulp mapping |
|---|---|
| `MTS_RegisterClient()` / `MTS_DeregisterClient()` | Construct/destroy `pulp::midi::MtsEspTuningProvider` |
| `MTS_NoteToFrequency(client, note, channel)` | `provider.note_to_frequency(note, channel).frequency_hz` |
| `MTS_RetuningInSemitones(client, note, channel)` | `provider.note_to_frequency(note, channel).retuning_semitones` |
| `MTS_RetuningAsRatio(client, note, channel)` | `provider.note_to_frequency(note, channel).retuning_ratio` |
| `MTS_ShouldFilterNote(client, note, channel)` | `provider.note_to_frequency(note, channel).should_filter_note` |
| `MTS_FrequencyToNote*` helpers | `provider.frequency_to_note(...)` or `provider.frequency_to_note_and_channel(...)` |
| `MTS_ParseMIDIDataU(...)` / `MTS_ParseMIDIData(...)` | `provider.parse_midi_data(...)` on the non-audio side of MIDI/SysEx handling |
| master/status helpers such as `MTS_HasMaster()` and `MTS_GetScaleName()` | `provider.status()` |

For JUCE imports, preserve the original `processBlock` policy: if the project
queried tuning only at note-on, call `note_to_frequency()` when allocating the
voice; if it refreshed continuously, query per block or at the original
retuning cadence. For iPlug2 imports, route `ProcessSysEx` / MIDI SysEx parsing
to `parse_midi_data()` and keep channel handling unchanged unless the original
code was explicitly MPE-aware.

## Audio Thread Rule

Frequency, retuning, and note-filter queries are intended for the processing
path. Status display, client construction/destruction, and parsing arbitrary
SysEx buffers should stay off the audio callback. For synth voices, expose a
mode that either latches tuning at note-on or continuously refreshes pitch
while the note is held.

## Runtime Behavior

The client source does not bundle the MTS-ESP master or libMTS shared library.
On desktop it tries to load the installed libMTS runtime from the conventional
MTS-ESP location. If none is installed, note queries still return local tuning
data and `parse_midi_data()` can update that local table from MTS SysEx.
Because the upstream client has a small file-scope loader, that dynamic-library
probe happens when the linked client code is loaded. Keep that boundary opt-in:
enable `PULP_ENABLE_MTS_ESP=ON` or link the `mts_esp_client` package target only
for products that want MTS-ESP support.

## License

MTS-ESP is 0BSD. It is fetched only when explicitly enabled with
`PULP_ENABLE_MTS_ESP=ON` or `pulp add mts-esp`, so it is not part of Pulp's
default dependency chain.
