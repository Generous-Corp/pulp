# G3 D5 bounded resolved identity bridge

This milestone adds the admitted `pulp.graph.resolved-plan.v1` /
`pulp.graph.resolved.io-gain.v1` projection and detached receipt APIs. The D1,
D2, and D4 canonical encoders remain unchanged. Actual plan and storage facts
are captured into typed value-owned records before the legacy diagnostic
strings are formatted; provenance and allocator slack remain outside portable
bytes.

Qualification evidence on this head is positive for the bounded local slice:
the governed host targets compile, the installed-style stereo positive passes,
and the D4 and graph-identity regression suites remain green. The resolved
canonical vector is 13,606 bytes with digest
`sha256:83f5e4e38d2b9610962fcf8c04a11cc11b511ec52d88bdbbb8db61110d7da48e`;
the digest is independently recomputed from `domain + NUL + bytes`. Planted
controls cover malformed receipts, forged endpoints, overlapping storage,
zero hard host limits, byte mutation, provenance changes, stale snapshots,
aliases, and owner destruction. The installed public-only Release consumer
remains PARTIAL because this checkout's generated SDK package lacks the
relocatable support modules. Upper-half unsigned-64 JSON values are safely
refused and remain a documented numerical-acceptance PARTIAL. Full
cross-platform and installed Release qualification remain required for whole
G3 closure.
