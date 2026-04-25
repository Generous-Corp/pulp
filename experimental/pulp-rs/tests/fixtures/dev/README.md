# `pulp-rs dev` integration fixtures

Phase 6d ports `pulp dev` as a build-once stub (no watch loop). The
integration test lives at `tests/dev_parity_test.rs`.

Because `dev` just configures + builds + optionally tests, a
cross-platform parity fixture of the shell output would bind us to a
given CMake version + build type. Instead, the parity test invokes
`pulp-rs dev --help` and asserts the banner contains the exact flag
strings from `cmd_dev.cpp`. That's the byte-parity surface — every
other line is `cmake` output the C++ CLI doesn't own either.
