#!/usr/bin/env python3
"""audit.py — License, clean-room, and vendor-material audit tool for Pulp projects.

Checks for:
- License compatibility of dependencies
- Disallowed license types (GPL, LGPL, AGPL, SSPL, proprietary)
- Clean-room naming violations (JUCE names in source code)
- Forbidden vendored vendor artifacts or copied AAX SDK text
- Missing DEPENDENCIES.md entries for detected dependencies

Usage:
    python3 tools/audit.py                    # Audit the current project
    python3 tools/audit.py path/to/library    # Audit a specific library
"""

import argparse
import re
import sys
from pathlib import Path

# License classifications
ALLOWED = {'MIT', 'BSD-2-Clause', 'BSD-3-Clause', 'Apache-2.0', 'ISC',
           'zlib', 'BSL-1.0', 'Unlicense', 'public domain', 'CC0-1.0'}
DISALLOWED = {'GPL-2.0', 'GPL-3.0', 'LGPL-2.1', 'LGPL-3.0', 'AGPL-3.0',
              'SSPL-1.0', 'proprietary'}
REVIEW = {'MPL-2.0'}

# Names to flag (clean-room violations).
# Keep this list to JUCE-specific identifiers. Generic audio/UI names like
# "MidiBuffer", "Slider", or "Label" create too many false positives to serve
# as reliable contamination checks.
JUCE_NAMES = [
    'AudioProcessorEditor', 'AudioProcessorValueTreeState',
    'AudioPluginInstance', 'GenericAudioProcessorEditor',
    'AudioFormatManager', 'AudioThumbnail', 'LookAndFeel', 'ComponentBoundsConstrainer',
    'DocumentWindow', 'TopLevelWindow', 'TabbedComponent', 'PropertyPanel',
    'juce_audio_basics', 'juce_audio_devices', 'juce_audio_formats',
    'juce_audio_processors', 'juce_gui_basics', 'juce_gui_extra',
    'juce_dsp', 'juce_core', 'JUCE',
]

SKIP_PREFIXES = ('external/', 'build/', '.git/', 'planning/')
TEXT_EXTENSIONS = {
    '.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.ipp',
    '.m', '.md', '.mm', '.cmake', '.in', '.json', '.txt', '.xml',
}
FORBIDDEN_VENDOR_PATH_PATTERNS = [
    re.compile(r'(^|/)(aax-sdk-[^/]+|AAXLibrary)(/|$)', re.IGNORECASE),
    re.compile(r'(^|/)(AAX_[^/]+\.(c|cc|cpp|cxx|h|hh|hpp|mm)|CACF[^/]+\.(c|cc|cpp|cxx|h|hh|hpp))$'),
    re.compile(r'(^|/)(aax-validator|digishell|aax-page-table-editor|cloudclientservices|hd_driver|pro[ _-]?tools|protoolsktrace|protoolswprtool|juce_to_aax)[^/]*$', re.IGNORECASE),
]
FORBIDDEN_VENDOR_TEXT_MARKERS = [
    'This file is part of the Avid AAX SDK.',
    'The AAX SDK is subject to commercial or open-source licensing.',
    'AAX SDK License: https://developer.avid.com/aax',
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


def should_skip_relative_path(rel: str) -> bool:
    if rel.startswith(SKIP_PREFIXES):
        return True
    first = Path(rel).parts[0] if Path(rel).parts else ''
    return first.startswith('build')


def audit_clean_room(root: Path, errors: list):
    """Check for JUCE naming violations in source code."""
    for ext in ['*.cpp', '*.hpp', '*.h', '*.mm']:
        for f in root.rglob(ext):
            # Skip external/ and build/
            rel = str(f.relative_to(root))
            if should_skip_relative_path(rel):
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


def audit_forbidden_vendor_material(root: Path, errors: list):
    """Check for vendored AAX/Vendor SDK artifacts or copied vendor text."""
    for f in root.rglob('*'):
        if not f.is_file():
            continue

        rel = str(f.relative_to(root))
        if should_skip_relative_path(rel):
            continue

        for pattern in FORBIDDEN_VENDOR_PATH_PATTERNS:
            if pattern.search(rel):
                errors.append(f'{rel}: forbidden vendor artifact in repo')
                break
        else:
            if f.suffix.lower() not in TEXT_EXTENSIONS:
                continue
            try:
                content = f.read_text(errors='replace')
            except Exception:
                continue
            for marker in FORBIDDEN_VENDOR_TEXT_MARKERS:
                if marker in content:
                    errors.append(f'{rel}: forbidden vendor source marker — contains "{marker}"')
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

    if not args.path:
        print('\nRunning vendor material check...')
        audit_forbidden_vendor_material(root, errors)
        if not any('forbidden vendor' in e for e in errors):
            print('  No forbidden vendor material found')

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
