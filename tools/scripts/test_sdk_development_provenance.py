import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import sdk_development_provenance as tool

SHA = 'a' * 40
HEADER = '''#pragma once
#include <string_view>
namespace pulp::runtime {
inline constexpr std::string_view kBuildType = "Release";
inline constexpr std::string_view kBuildIso8601 = "today";
inline constexpr std::string_view kGitSha = "aaaaaaaaaa";
inline constexpr bool kGitDirty = false;
inline constexpr std::string_view kSdkVersion = "0.876.1";
inline constexpr std::string_view kStampLabel = "test";
}
'''

def member(name, payload, timestamp='0'):
    return (name.ljust(16) + timestamp.ljust(12) + '0'.ljust(6) +
            '0'.ljust(6) + '100644'.ljust(8) + str(len(payload)).ljust(10) +
            '`\n').encode() + payload + (b'\n' if len(payload) % 2 else b'')


def archive(timestamp='0', payload=b'code'):
    return b'!<arch>\n' + member('__.SYMDEF', b'index', timestamp) + member('node.o/', payload)


class AdoptionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source, self.build, self.prefix = (self.root / p for p in ('source', 'build', 'prefix'))
        for p in (self.source, self.build, self.prefix):
            p.mkdir()
        self.cache = self.build / 'CMakeCache.txt'
        self.cache.write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n'
                              f'CMAKE_CACHEFILE_DIR:INTERNAL={self.build}\n'
                              'CMAKE_BUILD_TYPE:STRING=Release\nPULP_BUILD_WEBVIEW:BOOL=OFF\n')
        self.built_header = self.build / 'core/runtime/generated/Release/pulp/runtime/build_info.hpp'
        self.header = self.prefix / 'include/pulp/runtime/build_info.hpp'
        self.installed_archive = self.prefix / 'lib/libpulp-gpu-audio.a'
        for p, data in ((self.built_header, HEADER.encode()), (self.header, HEADER.encode()),
                        (self.build / 'core/gpu_audio/libpulp-gpu-audio.a', archive()),
                        (self.installed_archive, archive('123')),
                        (self.prefix / 'lib/cmake/Pulp/PulpConfig.cmake', b'config'),
                        (self.prefix / 'lib/cmake/Pulp/PulpTargets.cmake', b'targets'),
                        (self.build / 'PulpConfig.cmake', b'config'),
                        (self.prefix / 'version.txt', b'0.876.1\n'),
                        (self.prefix / 'sdk_build_type.txt', b'Release\n')):
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        self.mock = patch.object(tool, 'git', side_effect=lambda source, *args: SHA if args[0] == 'rev-parse' else '')
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def inspect(self):
        return tool.inspect(self.source, self.build, self.prefix, SHA)

    def test_truthful_development_accepts_only_index_timestamp_difference(self):
        doc = self.inspect()
        self.assertFalse(doc['distribution_eligible'])
        self.assertEqual(doc['cache_features']['PULP_BUILD_WEBVIEW'], 'OFF')
        self.assertEqual(doc['profile'], 'existing-build-experiment')
        tool.publish(self.prefix, doc)
        self.assertEqual((self.prefix / 'sdk-provenance.json').stat().st_mode & 0o777, 0o644)
        with self.assertRaises(FileExistsError):
            tool.publish(self.prefix, doc)

    def test_source_head_and_dirty_rejected(self):
        for output in ('b' * 40, ' M tracked'):
            with self.subTest(output=output), patch.object(tool, 'git', return_value=output):
                with self.assertRaises(tool.ProvenanceError):
                    self.inspect()

    def test_header_drift_rejected(self):
        self.header.write_text(HEADER.replace('aaaaaaaaaa', 'bbbbbbbbbb'))
        with self.assertRaisesRegex(tool.ProvenanceError, 'header drift'):
            self.inspect()

    def test_matching_wrong_headers_rejected(self):
        for p in (self.header, self.built_header):
            p.write_text(HEADER.replace('aaaaaaaaaa', 'bbbbbbbbbb'))
        with self.assertRaisesRegex(tool.ProvenanceError, 'source SHA'):
            self.inspect()

    def test_object_drift_rejected(self):
        self.installed_archive.write_bytes(archive('123', b'evil'))
        with self.assertRaisesRegex(tool.ProvenanceError, 'archive drift'):
            self.inspect()

    def test_non_index_timestamp_drift_rejected(self):
        self.installed_archive.write_bytes(b'!<arch>\n' + member('__.SYMDEF', b'index') + member('node.o/', b'code', '123'))
        with self.assertRaisesRegex(tool.ProvenanceError, 'archive drift'):
            self.inspect()

    def test_cache_source_drift_rejected(self):
        self.cache.write_text(self.cache.read_text().replace(str(self.source), str(self.prefix)))
        with self.assertRaisesRegex(tool.ProvenanceError, 'cache source'):
            self.inspect()

    def test_config_drift_rejected(self):
        (self.prefix / 'lib/cmake/Pulp/PulpConfig.cmake').write_text('wrong config')
        with self.assertRaisesRegex(tool.ProvenanceError, 'package config drift'):
            self.inspect()

    def test_marker_drift_rejected(self):
        for name, original in [('version.txt', '0.876.1'), ('sdk_build_type.txt', 'Release')]:
            with self.subTest(name=name):
                path = self.prefix / name
                path.write_text('wrong')
                with self.assertRaisesRegex(tool.ProvenanceError, 'marker drift'):
                    self.inspect()
                path.write_text(original)

    def test_unrelated_cache_entries_not_disclosed(self):
        with self.cache.open('a') as stream:
            stream.write('PULP_SECRET_TOKEN:STRING=never-publish-this\n')
        self.assertNotIn('never-publish-this', str(self.inspect()))

    def test_release_eligibility_rejected(self):
        for field, value in [('kind', 'release'), ('distribution_eligible', True)]:
            doc = self.inspect()
            doc[field] = value
            with self.assertRaisesRegex(tool.ProvenanceError, 'release eligibility'):
                tool.publish(self.prefix, doc)
        self.assertFalse((self.prefix / 'sdk-provenance.json').exists())


if __name__ == '__main__':
    unittest.main()
