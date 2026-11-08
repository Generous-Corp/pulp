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
