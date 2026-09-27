#!/usr/bin/env python3
"""Validate/adopt an existing Release SDK for local experiments, never distribution.

No compilation, release tags, or forge-dev profile claims. Check is read-only;
stamp exclusively creates a new marker and refuses to replace existing identity.
Archive comparison ignores only the symbol-index member's timestamp.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from sdk_provenance import ProvenanceError, parse_build_info, verify_build_info_text


# Explicit feature metadata only. Never serialize arbitrary cache values:
# checkout-specific cache entries may carry authentication material.
FEATURE_KEYS = (
    'PULP_ENABLE_GPU', 'PULP_HAS_SKIA', 'PULP_BUILD_WEBVIEW',
    'PULP_ENABLE_DESIGN_IMPORT', 'PULP_ENABLE_AUDIO_PROBES',
    'PULP_ENABLE_INSPECTOR', 'PULP_TRACING', 'PULP_HAS_VST3',
    'PULP_HAS_AUSDK', 'PULP_HAS_CLAP',
    'PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF',
    'PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER',
    'PULP_GPU_AUDIO_HAS_EXPERIMENTAL_SHARED_IO_CONVOLVER',
)

def require(value, message):
    if not value:
        raise ProvenanceError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive_identity(data):
    require(data.startswith(b'!<arch>\n'), 'not a regular ar archive')
    normalized = bytearray(data)
    offset, objects = 8, 0
    while offset < len(data):
        header = data[offset:offset + 60]
        require(len(header) == 60 and header[58:60] == b'`\n', 'invalid ar header')
        size = int(header[48:58].strip())
        require(size >= 0 and offset + 60 + size <= len(data), 'truncated ar member')
        name = header[:16].rstrip()
        if name.startswith(b'#1/'):
            length = int(name[3:])
            require(length <= size, 'invalid extended ar name')
            name = data[offset + 60:offset + 60 + length].rstrip(b'\0')
        if name in (b'/', b'/SYM64/', b'__.SYMDEF', b'__.SYMDEF SORTED',
                    b'__.SYMDEF_64', b'__.SYMDEF_64 SORTED'):
            normalized[offset + 16:offset + 28] = b' ' * 12
        elif name != b'//':
            objects += 1
        offset += 60 + size + size % 2
    require(offset == len(data) and objects > 0, 'empty or malformed archive')
    return digest(normalized)


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()


def inspect(source, build, prefix, sha):
    source, build, prefix = (p.resolve(strict=True) for p in (source, build, prefix))
    require(re.fullmatch('[0-9a-f]{40}', sha), 'expected exact source SHA')
    require(git(source, 'rev-parse', 'HEAD') == sha, 'source HEAD drift')
    require(not git(source, 'status', '--porcelain', '--untracked-files=no'), 'dirty source')
    cache_bytes = (build / 'CMakeCache.txt').read_bytes()
    cache = dict(re.findall(r'^([A-Za-z_][A-Za-z0-9_]*):[^=\n]+=(.*)$', cache_bytes.decode(), re.M))
    require(Path(cache['CMAKE_HOME_DIRECTORY']).resolve() == source, 'cache source mismatch')
    require(Path(cache['CMAKE_CACHEFILE_DIR']).resolve() == build, 'cache build mismatch')
    require(cache['CMAKE_BUILD_TYPE'] == 'Release', 'Release build required')
    generated = (build / 'core/runtime/generated/Release/pulp/runtime/build_info.hpp').read_bytes()
    installed = (prefix / 'include/pulp/runtime/build_info.hpp').read_bytes()
    require(generated == installed, 'installed/generated build header drift')
    info = parse_build_info(installed.decode())
    verify_build_info_text(installed.decode(), expected_version=info['kSdkVersion'],
                           expected_source_sha=sha)
    require((prefix / 'version.txt').read_text().strip() == info['kSdkVersion'],
            'installed version marker drift')
    require((prefix / 'sdk_build_type.txt').read_text().strip() == info['kBuildType'],
            'installed build type marker drift')
    require((build / 'PulpConfig.cmake').read_bytes() ==
            (prefix / 'lib/cmake/Pulp/PulpConfig.cmake').read_bytes(),
            'installed/generated package config drift')
    built_archive = (build / 'core/gpu_audio/libpulp-gpu-audio.a').read_bytes()
    installed_archive = (prefix / 'lib/libpulp-gpu-audio.a').read_bytes()
    require(archive_identity(built_archive) == archive_identity(installed_archive),
            'installed/build GPU archive drift')
    artifacts = {}
    for name in ('lib/libpulp-gpu-audio.a', 'include/pulp/runtime/build_info.hpp',
                 'lib/cmake/Pulp/PulpConfig.cmake', 'lib/cmake/Pulp/PulpTargets.cmake',
                 'version.txt', 'sdk_build_type.txt'):
        artifacts[name] = digest((prefix / name).read_bytes())
    require(git(source, 'rev-parse', 'HEAD') == sha and
            not git(source, 'status', '--porcelain', '--untracked-files=no'),
            'source changed during inspection')
    return {'schema': 'pulp.sdk-provenance.v1', 'kind': 'development',
            'profile': 'existing-build-experiment', 'distribution_eligible': False,
            'source_git_sha': sha, 'source_git_dirty': False,
            'sdk_version': info['kSdkVersion'], 'build_type': 'Release',
            'scope': 'GPU archive identity and installed consumer metadata; not full SDK certification',
            'source_dir': str(source), 'build_dir': str(build),
            'cache_sha256': digest(cache_bytes),
            'cache_features': {k: cache[k] for k in FEATURE_KEYS if k in cache},
            'artifacts_sha256': artifacts,
            'build_gpu_archive_sha256': digest(built_archive),
            'gpu_archive_identity_sha256': archive_identity(built_archive)}


def publish(prefix, document):
    require(document.get('kind') == 'development' and
            document.get('distribution_eligible') is False,
            'release eligibility is forbidden')
    target = prefix / 'sdk-provenance.json'
    # Hard-link publication is atomic and cannot replace an existing identity.
    fd, temporary = tempfile.mkstemp(prefix='.development-provenance-', dir=prefix)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(json.dumps(document, indent=2, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.link(temporary, target)
    finally:
        os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['check', 'stamp'])
    for name in ('source-dir', 'build-dir', 'prefix'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--source-sha', required=True)
    args = parser.parse_args()
    try:
        document = inspect(args.source_dir, args.build_dir, args.prefix, args.source_sha)
        if args.command == 'stamp':
            publish(args.prefix, document)
        print(json.dumps(document, indent=2, sort_keys=True))
    except (ProvenanceError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'development SDK provenance: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
