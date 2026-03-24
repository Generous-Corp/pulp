# Dependencies

All dependencies must be compatible with MIT licensing. No copyleft (GPL, LGPL, AGPL) dependencies in any subsystem.

## Policy

- **Allowed licenses:** MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, zlib, BSL-1.0, Unlicense, public domain
- **Not allowed:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case evaluation)

## Current Dependencies

| Name | Version | License | How Used | Subsystem | Added |
|------|---------|---------|----------|-----------|-------|
| — | — | — | — | — | — |

*Dependencies are added as implementation begins. This table is updated with every new dependency.*

## Format SDKs (Obtained by Developers)

These SDKs are not bundled but are required for specific plugin format support:

| SDK | License | Required For | Bundled? |
|-----|---------|-------------|----------|
| VST3 SDK | MIT | VST3 format | Will bundle |
| AudioUnit SDK | Apache-2.0 | AU/AUv3 format | Will bundle |
| CLAP | MIT | CLAP format | Will bundle |
| LV2 SDK | ISC | LV2 format | Will bundle |
| AAX SDK | Proprietary (Avid) | AAX format | Developer obtains independently |
| ASIO SDK | Proprietary (Steinberg) | ASIO audio I/O | Developer obtains independently |
