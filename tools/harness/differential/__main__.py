from __future__ import annotations

import argparse
import json
from pathlib import Path

from .contract import normalize_report


def main() -> int:
    parser = argparse.ArgumentParser(description="normalize a Pulp Canvas/SVG differential report")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--browser", type=Path, help="adapter observations JSON")
    parser.add_argument("--native", type=Path, help="adapter observations JSON")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    read = lambda p: json.loads(p.read_text(encoding="utf-8")) if p else {}
    text = normalize_report(args.manifest, browser=read(args.browser), native=read(args.native)).to_json()
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
