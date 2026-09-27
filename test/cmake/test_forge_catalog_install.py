#!/usr/bin/env python3
"""Exercise the real catalog install graph with a small controlled exporter."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--build-wrapper', type=Path)
args, remaining = parser.parse_known_args()
SOURCE = args.source.resolve()
CMAKE = shutil.which('cmake')
GPU = {'schema': 'pulp.forge-catalog.v1', 'nodes': [
    {'key': 'convolution_reverb', 'realizations': [{'mode': 'default'}, {'mode': 'gpu', 'type_id': 'space.convolution_reverb_gpu'}]}]}
CPU = {'schema': 'pulp.forge-catalog.v1', 'nodes': [
    {'key': 'convolution_reverb', 'realizations': [{'mode': 'default'}]}]}


class CatalogInstall(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pulp-forge-catalog-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.build, self.prefix = (self.root / x for x in ('source', 'build', 'prefix'))
        (self.source / 'docs/status').mkdir(parents=True)
        self.snapshot = self.source / 'docs/status/forge-catalog.json'
        self.snapshot.write_text(json.dumps(CPU) + '\n')
        self.payload = self.source / 'payload.json'
        self.payload.write_text(json.dumps(GPU) + '\n')
        (self.source / 'exporter.cpp').write_text(r'''
#include <fstream>
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    if (argc != 5 || std::string(argv[1]) != "forge" ||
        std::string(argv[2]) != "catalog" || std::string(argv[3]) != "export" ||
        std::string(argv[4]) != "--json") return 64;
    std::ifstream input(MOCK_PAYLOAD);
    if (!input) return 7;
    std::cout << input.rdbuf();
    return 0;
}
''')
        (self.source / 'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.24)
project(ForgeCatalogInstall LANGUAGES CXX)
if(NOT OMIT_EXPORTER)
    add_executable(pulp-cli exporter.cpp)
    target_compile_definitions(pulp-cli PRIVATE MOCK_PAYLOAD="${CMAKE_CURRENT_SOURCE_DIR}/payload.json")
endif()
include("${PULP_SOURCE_DIR}/tools/cmake/PulpForgeCatalogInstall.cmake")
pulp_install_forge_catalog()
enable_testing()
if(PULP_HOST_ENABLE_GPU_CONVOLUTION)
    pulp_add_generated_forge_catalog_check()
endif()
''')

    def command(self, command, success=True):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=120)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def configure(self, enabled=True, omit=False, success=True):
        return self.command([CMAKE, '-S', self.source, '-B', self.build,
                             '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={self.prefix}',
                             f'-DPULP_SOURCE_DIR={SOURCE}',
                             f'-DPULP_HOST_ENABLE_GPU_CONVOLUTION={"ON" if enabled else "OFF"}',
                             f'-DOMIT_EXPORTER={"ON" if omit else "OFF"}'], success)

    def install(self, success=True):
        command = [CMAKE, '--build', self.build, '--target', 'install']
        if args.build_wrapper:
            command.insert(0, str(args.build_wrapper))
        else:
            command += ['--parallel', '2']
        return self.command(command, success)

    @property
    def installed(self):
        return self.prefix / 'share/pulp/forge-catalog.json'

    def test_install_rules_delegate_catalog_selection(self):
        rules = (SOURCE / 'tools/cmake/PulpInstallRules.cmake').read_text()
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/PulpForgeCatalogInstall.cmake")', rules)
        self.assertIn('pulp_install_forge_catalog()', rules)
        self.assertNotIn('install(FILES "${_pulp_forge_catalog_snapshot}"', rules)

    def test_default_installs_committed_snapshot_without_exporter(self):
        self.configure(enabled=False, omit=True)
        self.install()
        self.assertEqual(self.installed.read_bytes(), self.snapshot.read_bytes())

    def test_embedded_default_uses_pulp_snapshot_not_consumer_snapshot(self):
        child = self.source / 'embedded-pulp'
        (child / 'docs/status').mkdir(parents=True)
        expected = dict(CPU, owner='embedded-pulp')
        (child / 'docs/status/forge-catalog.json').write_text(json.dumps(expected))
        self.snapshot.write_text(json.dumps(dict(CPU, owner='consumer')))
        (child / 'CMakeLists.txt').write_text('''
include("${PULP_SOURCE_DIR}/tools/cmake/PulpForgeCatalogInstall.cmake")
pulp_install_forge_catalog()
''')
        (self.source / 'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.24)
project(Consumer LANGUAGES NONE)
add_subdirectory(embedded-pulp)
''')
        self.configure(enabled=False, omit=True)
        self.install()
        self.assertEqual(json.loads(self.installed.read_text()), expected)

    def test_enabled_generates_installs_and_rebuilds_from_exporter(self):
        self.configure()
        self.install()
        self.assertEqual(json.loads(self.installed.read_text()), GPU)
        self.assertEqual(json.loads(self.snapshot.read_text()), CPU)
        ctest = [shutil.which('ctest'), '--test-dir', self.build,
                 '-R', '^cli-forge-catalog-check$', '--output-on-failure']
        self.command(ctest)
        # A drift unrelated to the presence of the GPU mode must fail too.
        generated = self.build / 'forge-catalog/Release/forge-catalog.json'
        generated.write_text(json.dumps(dict(GPU, incorrect_metadata=True)))
        result = self.command(ctest, success=False)
        self.assertIn('differs from the current runtime projection', result.stdout + result.stderr)
        updated = dict(GPU, test_generation=2)
        self.payload.write_text(json.dumps(updated))
        # A changed producer executable must invalidate its generated catalog.
        with (self.source / 'exporter.cpp').open('a') as f:
            f.write('\n// revised producer\n')
        self.install()
        self.assertEqual(json.loads(self.installed.read_text()), updated)
        self.command(ctest)

    def test_enabled_refuses_missing_exporter(self):
        result = self.configure(omit=True, success=False)
        self.assertIn('native pulp-cli exporter', result.stdout + result.stderr)

    def test_export_failure_does_not_install_snapshot_or_partial_output(self):
        self.configure()
        self.payload.unlink()
        result = self.install(success=False)
        self.assertIn('Forge catalog exporter failed', result.stdout + result.stderr)
        self.assertFalse(self.installed.exists())
        self.assertFalse(list((self.build / 'forge-catalog').rglob('*.tmp')))

    def test_cpu_only_export_is_rejected(self):
        self.configure()
        self.payload.write_text(json.dumps(CPU))
        result = self.install(success=False)
        self.assertIn('missing from the exported Forge catalog', result.stdout + result.stderr)
        self.assertFalse(self.installed.exists())

    def test_gpu_mode_with_wrong_type_is_rejected(self):
        self.configure()
        wrong = json.loads(json.dumps(GPU))
        wrong['nodes'][0]['realizations'][1]['type_id'] = 'space.convolution_reverb'
        self.payload.write_text(json.dumps(wrong))
        result = self.install(success=False)
        self.assertIn('missing from the exported Forge catalog', result.stdout + result.stderr)
        self.assertFalse(self.installed.exists())

    def test_malformed_export_is_rejected(self):
        self.configure()
        self.payload.write_text('{"broken":')
        result = self.install(success=False)
        self.assertIn('invalid catalog JSON', result.stdout + result.stderr)
        self.assertFalse(self.installed.exists())


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
