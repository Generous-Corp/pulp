# Managed Chrome for Testing archive verification — 2026-07-29

This is the independent provenance record for Pulp's committed Chrome for
Testing `151.0.7922.47` pins. The verification was run from `/tmp`, outside the
Pulp checkout, after the pins were written.

Authoritative upstream metadata:

- `https://googlechromelabs.github.io/chrome-for-testing/151.0.7922.47.json`
- version: `151.0.7922.47`
- revision: `1654411`
- downloaded metadata SHA-256:
  `ad4f6bd2cd18e9ea2528d7bb09af46bbd869541fcb331a059ae2a9d58019c0ca`

Google's version manifest publishes immutable archive URLs but does not publish
archive checksums. Each complete archive below was therefore downloaded again
from its manifest URL and hashed locally with `shasum -a 256`.

Verification procedure:

```bash
curl -fL -o chrome-<platform>.zip <official-manifest-url>
shasum -a 256 chrome-<platform>.zip
stat -f '%N %z bytes' chrome-<platform>.zip
```

| Platform | Official archive | Bytes | Independently observed SHA-256 |
|---|---|---:|---|
| `linux64` | `https://storage.googleapis.com/chrome-for-testing-public/151.0.7922.47/linux64/chrome-linux64.zip` | 193,274,825 | `14ac03a67e154e3f8bbc57e03ef03315fda8fedff8e045eee8b31500283a33f4` |
| `mac-arm64` | `https://storage.googleapis.com/chrome-for-testing-public/151.0.7922.47/mac-arm64/chrome-mac-arm64.zip` | 187,097,179 | `9529990b6afd9867a862c7a5bff2a4a8eef84614d910acac22e4c5fa5c24daee` |
| `mac-x64` | `https://storage.googleapis.com/chrome-for-testing-public/151.0.7922.47/mac-x64/chrome-mac-x64.zip` | 197,089,507 | `90f49258b8929867640ca59cf138191d25b4b34759e1509687e59a66be9ac99b` |
| `win64` | `https://storage.googleapis.com/chrome-for-testing-public/151.0.7922.47/win64/chrome-win64.zip` | 201,077,750 | `fc77bb98b550b7da23b14edfa282b59a022e7fdb075ac7625d2a5152ceb22396` |

Result: every independently observed digest exactly matched the corresponding
pin in `experimental/pulp-rs/src/cmd/chrome_for_testing.rs`.

The downloaded archives were verification scratch data under
`/tmp/pulp-cft-independent-verify-20260729/`; they are deliberately not part of
the repository or a Pulp distribution.
