# Forge Modular — licensing

Forge Modular is a set of Eurorack-style modules for [VCV Rack](https://vcvrack.com/),
generated with Forge and built on the [Pulp](https://github.com/Generous-Corp/pulp) audio SDK.

## This plugin

Copyright © 2026 Generous Corp. Released under the **MIT License** (see `LICENSE-MIT.txt`).

## Relationship to VCV Rack — read this if you intend to sell a module

VCV Rack is licensed **GPLv3**, with a **"VCV Rack Non-Commercial Plugin License Exception"**
granted under section 7 of the GPLv3. Quoting Rack's own `LICENSE.md`:

> You are granted the permission to use this software's Application Programming Interface (API)
> in your Plugin in source and binary forms, as well as link to this software with the Plugin,
> regardless of the Plugin's license terms even if it would otherwise violate the terms of this
> software's GPLv3, **provided that the Plugin is distributed free of charge.**

Three consequences, in order of how likely they are to catch someone out:

1. **This plugin is distributed free of charge**, which is what permits it to carry the MIT
   licence above rather than GPLv3. That is not incidental — it is the condition.
2. **Selling a Rack plugin requires a commercial licence from VCV** (support@vcvrack.com). It is
   included automatically for plugins sold through the [VCV Library](https://library.vcvrack.com/).
   **If you generate a module with Forge and then sell it, that obligation is yours**, as the
   distributor — not Forge's, and not Generous Corp's.
3. **No VCV Component Library graphics are used here.** Rack's stock knobs, ports and switches are
   licensed CC BY-NC 4.0, and its Core panel designs CC BY-NC-ND 4.0, which would restrict
   commercial use and forbid derivatives. Every panel graphic in this plugin is drawn by us, so
   that path stays clear.

The Rack SDK itself is **never redistributed** with this plugin. It is supplied by the developer
at build time and linked against; the shipped artifact contains no Rack source.

"VCV" is a trademark of VCV and is not used in this product's name. The phrase *"for VCV Rack"* is
explicitly permitted by VCV for promotion and is used in that sense only.

## Third-party components

| Component | Licence | How it is used |
|---|---|---|
| [Pulp](https://github.com/Generous-Corp/pulp) | MIT | The audio SDK this plugin is built on — DSP, format layer, panel tooling |
| [VCV Rack SDK](https://vcvrack.com/) | GPLv3 + Non-Commercial Plugin License Exception | Linked, never redistributed; see above |
| [Inter](https://rsms.me/inter/) | SIL Open Font License 1.1 | Panel lettering. Glyphs are **converted to outlines** at build time; no font file ships in the plugin. OFL §1 permits this, and the reserved-font-name clause is not engaged because no modified font is distributed. |
| [nanosvg](https://github.com/memononen/nanosvg) | zlib | Panel SVG rasterization (also Rack's own renderer) |
| [Zstandard](https://github.com/facebook/zstd) | BSD-3-Clause / GPLv2 dual | `.vcvplugin` packaging |

The mono-line panel alphabet in `design/glyphs.json` and the Ink & Signal panel design system are
original works commissioned for this product.

Full licence texts for Pulp's own dependency tree are in the SDK's `NOTICE.md`.
