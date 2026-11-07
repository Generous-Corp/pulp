# A10 routing/hosting API audit — 2026-11-06

This is a durable dependency/status artifact, not an installed-consumer receipt.
It records the exact current-main and dependency heads and the public API audit
that determines the next safe implementation slice.

- `origin/main=2c2164d2da39a32a12559d2cc1598aae2628440d`
- `G-1 merge=e19f556e67129117ef3a4587bff11fdee16ebf44`
- `G-9 merge=89c9be945120b9553f2983d265677292d4876caa`
- `installed_consumer=BLOCKED`
- `hostile_policy=HOLD`
- `next_action=clean exact-head SDK install, then external public consumer`
- `exact_head_installed_consumer_evidence=UNAVAILABLE`

The graph status file contains the complete API-to-evidence mapping and the
reason an older SDK cannot be substituted.
