# Presets

`pulp::state::PresetManager` saves, lists and loads presets for a plug-in's
`StateStore`. A preset is one JSON file holding parameter values plus optional
descriptive metadata.

## File format (schema 2)

```json
{
  "schema": 2,
  "name": "Warm Pad",
  "manufacturer": "Acme",
  "plugin": "Synth",
  "version": 3,
  "author": "A. Person",
  "description": "Slow attack, wide chorus",
  "tags": ["pad", "warm"],
  "program": 12,
  "parameters": { "Gain": -3.0, "Mix": 75.0 }
}
```

- `schema` is written by `save()`; a file without it is a schema 1 file and
  loads unchanged. Loading never rewrites a file.
- `author`, `description`, `tags` and `program` are optional author-supplied
  metadata. They are read when presets are listed (`PresetInfo`) and when a
  preset is prepared. `program` is a hint for default ordering and is not a MIDI
  program number by itself.
- Parameter names are JSON-escaped, so a name containing a quote or backslash
  round-trips. Files are parsed as strict JSON first; a file that is not valid
  JSON falls back to a key scan so hand-edited schema 1 files keep loading.

```cpp
PresetMetadata meta;
meta.author = "A. Person";
meta.tags = {"pad", "warm"};
presets.save("Warm Pad", "", meta);
```

## Loading: prepare, then apply

`load(path)` is `prepare(path)` followed by `apply(prepared)`.

- `prepare()` does all file reading, parsing, name resolution and range
  clamping and never touches the store. Call it from any non-audio thread.
  The result lists `unknown_params` (names in the file that match no
  parameter, strict JSON only) and `missing_params` (parameters the file did
  not mention; they keep their current value).
- `apply()` writes each value with `StateStore::set_value` on the calling
  thread, so main-thread listeners fire immediately, then updates
  `current_preset_name()` and `on_preset_loaded`.
- `PreparedPreset::apply_rt()` writes the same values as one
  `StateStore::set_values_rt` batch. It is real-time safe, and main-thread
  listeners are delivered on the next `pump_listeners()`. Values were clamped
  at prepare time.

Never read a file or parse JSON on the audio thread; prepare first.

## Listing

`all_presets()`, `factory_presets()` and `user_presets()` walk the preset
folders on each call. Metadata is parsed once per file and cached by path,
modification time and size, so an unchanged file is not read again;
`metadata_parse_count()` exposes the number of parses.

## Library: favourites, tags, search

`PresetLibrary` wraps a `PresetManager` with a searchable index and per-user
edits that survive content updates.

```cpp
PresetLibrary library(presets, PresetLibrary::default_overlay_path("Acme", "Synth"));

library.set_favourite("factory:Pads/Warm", true);
library.add_tag("factory:Pads/Warm", "mine");

PresetQuery query;
query.text = "warm pad";
query.favourite = true;
for (const auto& item : library.query(query).items)
    show(item.display_name, item.tags);
```

- **Keys.** Each preset has a stable key: `factory:<path>`, `user:<path>` or
  `content:<pack>:<path>`, where `<path>` is the path below the source root
  without the extension. A pack's version is not part of its key, so updating
  a pack keeps the edits made to its presets. A rename or move changes the
  key; the library re-links the edits when exactly one preset in the same
  source has the same content digest, and otherwise keeps them as orphans
  (`orphan_count()`), so they return with the preset.
- **Edits are separate from preset files.** Favourites, tag edits and display
  names are stored in one overlay file shared by every instance and format of
  the plug-in. A display name changes how a preset is shown, never its file.
  Effective tags are the file's tags plus added tags minus removed tags,
  compared case-insensitively.
- **Where the overlay lives.** `default_overlay_path()` is under the per-user
  application data folder, never inside the preset folders: the preset scanner
  lists every `*.json` below them, so an overlay there would appear as a
  preset. With an empty path, or when the file cannot be written,
  `capabilities().persistence` is false and edits last for the session.
- **Concurrent writers.** Each edit re-reads the file under a lock and merges
  per record, newest edit wins, so two instances editing at once keep each
  other's changes. `refresh()` picks up edits written elsewhere.
- **Search.** `query()` reads the in-memory index, never the disk; call
  `refresh()` after files change. Every word of `text` must match the name,
  folder, tags, author or description, case-insensitively. With text, results
  rank by name prefix, then word start, then substring, then other fields;
  ties and the no-text case follow `PresetQuery::sort`, then display name and
  key, so the order is deterministic. `generation` in the result changes
  whenever the index or any edit changes.
- **Threading.** The library serialises its own calls with a mutex but is not
  real-time safe; the audio thread must not call it.

`PULP_PRESET_LIBRARY_API_VERSION` identifies the surface for code that must
compile against both older and newer SDKs.

## Program numbers: Bank Select and Program Change

A plug-in can let a DAW, a controller or a sequencer choose presets by MIDI
number. This is opt-in: `PresetLibrary::enable_program_map()` gives every
preset a slot and saves the assignments with the other edits; a library that
never calls it writes no map.

- **Numbers.** A bank is a 14-bit number (Bank Select MSB * 128 + LSB) of up to
  128 program slots. Factory presets fill bank 0, user presets bank 1 and each
  content pack the next free bank. A source with more than 128 presets spills
  into a further bank.
- **Stable.** Assignments are keyed by preset key and are made once. An update
  that adds presets gives the newcomers free slots and moves none of the
  others; a preset that goes missing leaves its slot empty (a Program Change to
  it does nothing) and it answers to the same number when it returns. A slot you
  clear stays empty. Edit the map with `assign_program`, `clear_program`,
  `set_bank_name` and `renumber_bank`; look a number up with `preset_at()`, or a
  preset's number with `program_of()`. `PresetQuery::bank` and
  `PresetSort::program` list a bank in program order.
- **Renames.** A renamed preset keeps its slot only when the library can
  re-link it (it has an edit with a recorded content digest, see above).
  Otherwise its old slot is empty and the renamed file gets a new one.

`format::PresetProgramRouter` turns incoming MIDI into load requests without
loading anything on the audio thread:

```cpp
// audio thread, each block
router.process(input_midi);

// message thread
if (auto request = router.take_request())
    if (auto preset = library.preset_at(request->position))
        presets.load(preset->path);
```

- Bank Select (CC 0, CC 32) is latched per channel and applies to the next
  Program Change. `BankSelectMode` chooses 14-bit, MSB-only (the bank is CC 0
  alone) or ignored (always bank 0). `channel` is 0 for every channel or 1 to 16
  for one.
- A burst of Program Changes leaves only the newest request. `take_request()`
  empties the slot, and `sequence` counts every Program Change seen.
- Reset All Controllers (CC 121) does not clear the latched bank, as General
  MIDI specifies; `reset()` does.
- The router only observes: the MIDI buffer is untouched, so a processor that
  handles Program Change itself still sees it. `claims_controller()` tells a
  controller-mapping feature which controller numbers the router is using.
- `process()` and `on_message()` allocate nothing and take no lock.
- The router implements `state::MidiSourceReservation`: while enabled it claims
  Bank Select (CC 0 and CC 32, and the 14-bit pair on CC 0, in the modes that
  read them) and every Program Change, on the channels it listens to. A feature
  that maps controllers, such as MIDI Learn, asks `claims()` to decline those
  sources and shows `reason()` instead.

A Program Change applied from the message thread takes effect after the block
that carried it, so a note in the same block plays with the old preset.

### Opting in from a plug-in (CLAP)

Set `supports_program_change = true` in the `PluginDescriptor`. The CLAP
adapter then creates a `PresetLibrary` for the instance (edits stored under the
per-user application data folder), pins the program map, scans incoming MIDI
for Bank Select + Program Change on the audio thread, and loads the matching
preset from `on_main_thread` after asking the host for a callback. Bank
Select is 14-bit and every channel is listened to. A plug-in that does not set
the flag is unchanged: no library, no scan, no callback request.

The adapter also answers `clap.preset-load` at the plug-in location
(`CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN`) with a library key as the load key,
for example `factory:Pads/Warm`. Other formats do not use the flag yet.

## Preset browser: library mode

`PresetBrowser::set_library(&library)` switches the browser to the library's
query results; `set_library(nullptr)` restores the plain list. In library mode
each row shows a favourite star, the display name (a rename never touches the
file), the tags and the MIDI program number when a program map is enabled.

- Click the star column or press `F` to toggle the favourite of the selected
  row; Cmd/Ctrl+F is left alone. The selected row stays selected across edits.
- The search text uses the library grammar: every word must match the name,
  folder, tags, author or description. `set_favourites_only` and
  `set_tag_filter` narrow it further.
- `rename_selected`, `add_tag_to_selected` and `remove_tag_from_selected` are
  for a host dialog or a script; the browser has no text-entry field of its own.
- Preset callbacks still receive a `PresetInfo` carrying the file's own name, so
  loading is unchanged. `selected_summary()` returns the library's view of the
  row (key, display name, tags, favourite, program).

Without a library the browser is unchanged.

### Context menu and program editor

In library mode a right-click on a browser row selects it and calls
`PresetBrowser::on_context_menu(position, items)`. Show the items with
`ContextMenu::show` and pass the chosen id to `perform_menu_action`. The menu
offers favourite on/off, rename, add tag, remove each tag, assign a MIDI program
and clear it.

The browser and the editor have no text field. Actions that need text (rename,
add tag, a program as `bank:program`) call `on_text_request`; the host shows its
own dialog and answers through `complete_text_request`. An empty rename clears the
override, an empty tag is refused, and a program that is not `bank:program` in
range (`parse_bank_program`) is refused.

`PresetProgramEditor` shows one bank's 128 slots with the preset in each. The
arrows in its header, the Left and Right keys, or `next_bank`/`previous_bank`
change bank; Up and Down move the selected program and scroll it into
view, and the wheel scrolls the list; Delete clears it; a double
click or Enter calls `on_assign_request(slot)` so the host can pick a preset and
answer with `complete_assign(key)`. A double click on the title asks for a bank
name through `on_bank_text_request`, and `complete_bank_text` renames or
renumbers it. A slot whose preset is not installed is shown with its key and
"(not installed)". With no program map enabled the editor shows "(no banks)".

### Opting in from a plug-in (standalone)

The standalone host honours `supports_program_change` the same way. It creates
the library and router when the processor is created, scans hardware MIDI for
Bank Select + Program Change inside the audio callback (latch only), and loads
the preset from `StandaloneApp::service_program_changes()`, which the built-in
editors call from their idle loop and which throttles itself to about 30 Hz. A
host that runs its own loop calls it a few times a second. The factory folder is
`Presets` inside the app bundle's resource directory when it exists.

