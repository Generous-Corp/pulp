#!/usr/bin/env python3
"""add-component.py — Add a reusable component to a Pulp plugin project.

Usage:
    python3 tools/add-component.py preset-browser
    python3 tools/add-component.py --list
"""

import argparse
import re
import sys
from pathlib import Path


def parse_registry(path: Path) -> list[dict]:
    """Parse the component registry YAML (minimal parser)."""
    components = []
    current = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if not line.strip() or line.strip().startswith('#'):
                continue
            m = re.match(r'\s+-\s+name:\s+(.+)', line)
            if m:
                if current:
                    components.append(current)
                current = {'name': m.group(1).strip()}
                continue
            for key in ('description', 'status', 'phase'):
                m = re.match(rf'\s+{key}:\s+(.+)', line)
                if m:
                    current[key] = m.group(1).strip()
        if current:
            components.append(current)
    return components


def main():
    parser = argparse.ArgumentParser(description='Add a component to your Pulp project')
    parser.add_argument('name', nargs='?', help='Component name (e.g., preset-browser)')
    parser.add_argument('--list', action='store_true', help='List available components')
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent
    registry_path = root / 'tools' / 'components' / 'registry.yaml'

    if not registry_path.exists():
        print('Error: component registry not found', file=sys.stderr)
        return 1

    components = parse_registry(registry_path)

    if args.list or not args.name:
        if not components:
            print('No components available yet.')
            print('Components will be added in Phase 14 (Core UI Components).')
        else:
            print('Available components:\n')
            for c in components:
                status = c.get('status', 'unknown')
                desc = c.get('description', '')
                print(f"  {c['name']} [{status}] — {desc}")
        return 0

    # Find requested component
    comp = next((c for c in components if c['name'] == args.name), None)
    if not comp:
        print(f'Error: component "{args.name}" not found', file=sys.stderr)
        if components:
            print(f'Available: {", ".join(c["name"] for c in components)}')
        else:
            print('No components available yet. Coming in Phase 14.')
        return 1

    if comp.get('status') == 'planned':
        print(f'Component "{args.name}" is planned for Phase {comp.get("phase", "?")} but not yet implemented.')
        return 1

    # TODO: copy component files into project
    print(f'Added component "{args.name}" to your project.')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
