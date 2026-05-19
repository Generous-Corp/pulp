# tools/debug — Debugger formatters

Pretty-printers for Pulp's core types so you don't have to squint at
raw struct bytes in the debugger.

| File | For | Loaded by |
|---|---|---|
| `pulp.natvis` | Visual Studio / Windows debuggers | drop into `%USERPROFILE%\Documents\Visual Studio 20XX\Visualizers\` |
| `pulp_lldb.py` | LLDB on macOS / Linux / iOS | `command script import /path/to/pulp/tools/debug/pulp_lldb.py` in `~/.lldbinit` |

Types covered today:

* `pulp::state::ParamValue` — atomic float + atomic mod offset
* `pulp::state::ParamInfo` / `ParamRange` — parameter metadata
* `pulp::state::StateStore` — surfaces param count
* `pulp::state::ListenerToken` — RAII subscription handle
* `pulp::canvas::Rect` / `Point` / `Color`
* `pulp::audio::BufferView<T>` — channels × samples, LLDB version
  draws a 16-sample sparkline of the first channel

Adding a new type? Match its name in both files and keep them in
sync — the natvis side runs MSVC's expression engine and the LLDB
side runs Python, so they're parallel implementations, not shared
code. Per sudara "Big List of JUCE Tips" #21.
