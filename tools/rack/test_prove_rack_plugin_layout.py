#!/usr/bin/env python3
"""Unit-test the isolated Rack user directory used by the live proof."""

import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prove_rack_opens as proof  # noqa: E402


def main():
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        plugins = root / "source"
        plugin = plugins / "ForgeModular"
        plugin.mkdir(parents=True)
        (plugin / "plugin.json").write_text(json.dumps({"modules": [
            {"slug": "ARCBEAT", "name": "ARCBEAT"}]}))
        patch = root / "arcbeat.vcv"
        patch.write_text("{}")
        rack = root / "rack-stub.py"
        rack.write_text("#!/usr/bin/env python3\n"
                        "import sys\nfrom pathlib import Path\n"
                        "user = Path(sys.argv[sys.argv.index('-u') + 1])\n"
                        "link = user / 'plugins-mac-arm64' / 'ForgeModular'\n"
                        "if not link.is_symlink(): raise SystemExit(23)\n"
                        "Path(user, 'log.txt').write_text(\n"
                        "    'Loaded plugin ForgeModular 2.0.0\\n'\n"
                        "    'Creating module Forge ARCBEAT\\n')\n")
        rack.chmod(0o755)
        old_rack = proof.RACK
        old_settle = os.environ.get("PROVE_RACK_SETTLE")
        try:
            proof.RACK = rack
            os.environ["PROVE_RACK_SETTLE"] = "0"
            created, _ = proof.rack_creates(patch, plugins)
        finally:
            proof.RACK = old_rack
            if old_settle is None:
                os.environ.pop("PROVE_RACK_SETTLE", None)
            else:
                os.environ["PROVE_RACK_SETTLE"] = old_settle
    if created != {"Forge ARCBEAT": 1}:
        print(f"WRONG: {dict(created)}")
        return 1
    print("ok: isolated Rack loads the supplied architecture-specific plugin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
