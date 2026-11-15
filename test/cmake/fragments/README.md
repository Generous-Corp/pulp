# Registration fragments

One file per new test registration, named `<area>-<what>.cmake`.
`test/cmake/fragments.cmake` includes every `*.cmake` here after the owner
manifests, in lexical order. Do not append a block to a shared manifest
(`mac_platform_tests.cmake` refuses it at push); two pull requests that append
to the same file conflict, and the union of both sides is not a repair.
