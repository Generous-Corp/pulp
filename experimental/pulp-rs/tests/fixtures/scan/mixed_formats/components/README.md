# Scan-parity AU fixture

Two empty `.component` bundles plus a `Contents/MacOS/` stub so git
tracks the directory. The scan code only enumerates by directory
name (ending in `.component`), not by bundle contents — so empty
is fine for the parity test.

The `.gitkeep` files are the sentinel that keeps the nested
directories under version control. Without them, the `components/`
tree vanishes on fresh clones and the scan test fails with
"missing AU header".
