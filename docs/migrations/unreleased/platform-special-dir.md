---
breaking = false
summary = "special_dir() replaces private per-subsystem path lookups"
---

## What changed
- `pulp/platform/special_dir.hpp` adds `special_dir(SpecialDir)` for the user's
  home, documents, application data, config, cache, logs, temp, bundle resources
  and preset folders on every platform.
- Application settings, `PresetManager`, `ContentRegistry` and the view layer's
  persisted stores now resolve their roots through it.

## Migration steps
- Code that duplicated these lookups can call `special_dir` instead.
- On Linux the settings and preset roots now ignore a relative `XDG_CONFIG_HOME`,
  and `ApplicationProperties` honours an absolute `XDG_CONFIG_HOME` instead of
  always using `~/.config`. Other locations are unchanged.
- On Android the content root now lives under the app files directory instead of
  the working directory.
