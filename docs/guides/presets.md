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
