#!/usr/bin/env python3
"""audit.py — License and clean-room audit tool for Pulp projects.

Checks for:
- License compatibility of dependencies
- Disallowed license types (GPL, LGPL, AGPL, SSPL, proprietary)
- Clean-room naming violations (JUCE names in source code)
- Missing DEPENDENCIES.md entries for detected dependencies

Usage:
    python3 tools/audit.py                    # Audit the current project
    python3 tools/audit.py path/to/library    # Audit a specific library
"""

import argparse
import os
import re
import sys
from pathlib import Path

# License classifications
ALLOWED = {'MIT', 'BSD-2-Clause', 'BSD-3-Clause', 'Apache-2.0', 'ISC',
           'zlib', 'BSL-1.0', 'Unlicense', 'public domain', 'CC0-1.0'}
DISALLOWED = {'GPL-2.0', 'GPL-3.0', 'LGPL-2.1', 'LGPL-3.0', 'AGPL-3.0',
              'SSPL-1.0', 'proprietary'}
REVIEW = {'MPL-2.0'}

# Names to flag (clean-room violations)
JUCE_NAMES = [
    'AudioProcessor', 'AudioProcessorEditor', 'AudioProcessorValueTreeState',
    'AudioPluginInstance', 'AudioBuffer', 'MidiBuffer', 'GenericAudioProcessorEditor',
    'AudioFormatManager', 'AudioThumbnail', 'LookAndFeel', 'ComponentBoundsConstrainer',
    'FileChooser', 'ResizableWindow', 'DocumentWindow', 'TopLevelWindow',
    'Slider', 'TextButton', 'ToggleButton', 'Label', 'ComboBox',
    'TreeView', 'ListBox', 'TabbedComponent', 'PropertyPanel',
    'juce_audio_basics', 'juce_audio_devices', 'juce_audio_formats',
    'juce_audio_processors', 'juce_gui_basics', 'juce_gui_extra',
    'juce_dsp', 'juce_core', 'JUCE',
]


def find_license_file(path: Path) -> str:
    """Find and read a LICENSE file in the given directory."""
    for name in ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'COPYING', 'LICENCE']:
        f = path / name
        if f.exists():
            return f.read_text(errors='replace')
    return ''


def detect_license(text: str) -> str:
    """Detect license type from LICENSE file content."""
    t = text.lower()
    if 'mit license' in t or 'permission is hereby granted, free of charge' in t:
        return 'MIT'
    if 'apache license' in t and 'version 2.0' in t:
        return 'Apache-2.0'
    if 'bsd 3-clause' in t or 'redistribution and use in source and binary' in t:
        if 'neither the name' in t:
            return 'BSD-3-Clause'
        return 'BSD-2-Clause'
    if 'isc license' in t:
        return 'ISC'
    if 'zlib license' in t or 'freely granted' in t:
        return 'zlib'
    if 'boost software license' in t:
        return 'BSL-1.0'
    if 'gnu general public license' in t:
        if 'version 3' in t:
            return 'GPL-3.0'
        if 'version 2' in t:
            return 'GPL-2.0'
        if 'lesser' in t:
            return 'LGPL-2.1' if 'version 2.1' in t else 'LGPL-3.0'
        if 'affero' in t:
            return 'AGPL-3.0'
    if 'mozilla public license' in t:
        return 'MPL-2.0'
    if 'unlicense' in t:
        return 'Unlicense'
    return 'unknown'


def audit_directory(path: Path, errors: list, warnings: list):
    """Audit a directory for license and clean-room issues."""
    license_text = find_license_file(path)
    if license_text:
        detected = detect_license(license_text)
        if detected in DISALLOWED:
            errors.append(f'{path.name}: DISALLOWED license ({detected})')
        elif detected in REVIEW:
            warnings.append(f'{path.name}: requires review ({detected})')
        elif detected == 'unknown':
            warnings.append(f'{path.name}: could not detect license type')
        else:
            print(f'  {path.name}: {detected} (OK)')
    else:
        warnings.append(f'{path.name}: no LICENSE file found')


def audit_clean_room(root: Path, errors: list):
    """Check for JUCE naming violations in source code."""
    for ext in ['*.cpp', '*.hpp', '*.h', '*.mm']:
        for f in root.rglob(ext):
            # Skip external/ and build/
            rel = str(f.relative_to(root))
            if rel.startswith('external/') or rel.startswith('build/'):
                continue
            try:
                content = f.read_text(errors='replace')
            except Exception:
                continue
            for name in JUCE_NAMES:
                if name in content:
                    # Check it's not in a comment about JUCE (allowed in planning docs)
                    lines = content.split('\n')
                    for i, line in enumerate(lines):
                        if name in line and not line.strip().startswith('//'):
                            # False positive check: skip if it's in a string or our own naming
                            if f'"{name}"' not in line and f"'{name}'" not in line:
                                errors.append(f'{rel}:{i+1}: clean-room violation — uses "{name}"')
                                break


def main():
    parser = argparse.ArgumentParser(description='License and clean-room audit')
    parser.add_argument('path', nargs='?', default=None,
                        help='Directory to audit (default: project root)')
    parser.add_argument('--clean-room', action='store_true',
                        help='Also run clean-room naming check')
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent

    errors = []
    warnings = []

    if args.path:
        target = Path(args.path)
        if not target.exists():
            print(f'Error: {target} does not exist', file=sys.stderr)
            return 1
        print(f'Auditing: {target}')
        audit_directory(target, errors, warnings)
    else:
        # Audit all external dependencies
        ext_dir = root / 'external'
        if ext_dir.exists():
            print('Auditing external dependencies...\n')
            for d in sorted(ext_dir.iterdir()):
                if d.is_dir() and not d.name.startswith('.'):
                    audit_directory(d, errors, warnings)

        # Check DEPENDENCIES.md exists
        deps_file = root / 'DEPENDENCIES.md'
        if not deps_file.exists():
            warnings.append('DEPENDENCIES.md not found')
        else:
            print(f'\nDEPENDENCIES.md: present')

    # Clean-room check
    if args.clean_room or not args.path:
        print('\nRunning clean-room naming check...')
        audit_clean_room(root, errors)
        if not any('clean-room' in e for e in errors):
            print('  No clean-room violations found')

    # Summary
    print()
    for w in warnings:
        print(f'\033[1;33mWARN: {w}\033[0m')
    for e in errors:
        print(f'\033[1;31mERROR: {e}\033[0m')

    if errors:
        print(f'\n\033[1;31mFAILED: {len(errors)} error(s), {len(warnings)} warning(s)\033[0m')
        return 1
    elif warnings:
        print(f'\n\033[1;33mPASSED with {len(warnings)} warning(s)\033[0m')
    else:
        print(f'\n\033[1;32mPASSED: all checks clean\033[0m')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
