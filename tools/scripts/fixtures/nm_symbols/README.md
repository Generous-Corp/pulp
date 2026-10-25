# nm fixtures for the archive symbol gates

Inputs for `test_nm_symbols.py`, `test_view_audio_symbol_floor.py` and
`test_archive_symbol_closure_check.py`. Each `*.P.txt` is `nm -A -g -P`
output and each `*.m.txt` is `nm -g -m` (darwin format) output.

- `darwin_view_audio.*`, `darwin_view_core.*`: excerpts of Apple nm output
  for Pulp's own `libpulp-view-audio.a` and `libpulp-view-core.a` (macOS
  arm64 Release), cut to the Meter members, ChannelStripView, a
  `std::function` base over a lambda and `std::format_error`.
- `darwin_compiled_*`, `elf_view_*`: Apple nm and llvm-nm output for
  `view_audio.cpp`, `view_core.cpp` and `planted.cpp` in this directory,
  compiled with `clang++ -std=c++20 -fno-exceptions -O0 -c`, natively for
  Mach-O and with `-target x86_64-unknown-linux-gnu` for ELF, archived with
  `ar` / `llvm-ar`. `planted.cpp` is a view-core translation unit that sees
  Meter's definition, so its archive carries weak copies of Meter's RTTI.
- `elf_ci_shared_rtti_*`: the twenty weak RTTI symbols a Linux CI build of
  view-core shares with view-audio (std::format and choc classes, a lambda,
  view-core's own CustomShaderHost), each listed as GNU nm prints a weak
  object (`V`). None belongs to a class view-audio owns, so none may be
  flagged.
