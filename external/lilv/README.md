# lilv and its dependencies

The LV2 host loader in `core/host` reads plug-in descriptions, instantiates
plug-ins, and saves and restores their state through lilv. lilv needs serd,
sord, sratom, and zix, so all five are vendored here, unmodified, from the
author's release tarballs at https://download.drobilla.net/.

| Library | Version | Tarball SHA-256 |
|---------|---------|-----------------|
| lilv    | 0.28.0  | `8dcb70adb5cf072335115a6b091f4113710bdc73abaadaa3f9e9c1e55957b149` |
| serd    | 0.32.10 | `b0e93b49e52f01a049475b7886ef140407115a32d3b1e5dc5f95141c88275d1c` |
| sord    | 0.16.22 | `bb23b34b216579136795d518cffa73d91cf205594ce9accebfd408afb839173f` |
| sratom  | 0.6.22  | `0209b7d0f22c96abb416722ed735b0933be47931ecff4aa4b26ded7760b4f252` |
| zix     | 0.8.2   | `4c73aab0f8cbbfe56b00c8c6d648316021e699c5ae6cbf254391ef309047e67b` |

`vendored.json` records each library's version, tarball SHA-256, the upstream
files left out, and a digest of its vendored tree. CI runs
`tools/deps/test_verify_vendored_lilv.py`, which fails if any vendored byte
changes. `python3 tools/deps/verify_vendored_lilv.py --verify-upstream`
downloads the tarballs and checks that each one still produces the recorded
tree.

All five are ISC licensed; each directory keeps its `COPYING`. Only the library
sources (`src/`) and public headers (`include/`) are kept. The command-line
tools (`serdi`, `sordi`, `sord_validate`), tests, and build files are not.

`core/host/CMakeLists.txt` builds them as one static object library with hidden
symbols, so they never clash with a copy of lilv that a hosted plug-in or the
host application loads itself. To update, replace a directory's `include/`,
`src/`, and `COPYING` from a newer tarball, delete the tool sources again, and
update the table above, `vendored.json` (`--verify-upstream`, then `--write`),
and `DEPENDENCIES.md`.
