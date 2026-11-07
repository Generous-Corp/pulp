"""Run the actual installed CLI and MCP against a public consumer's fixture."""
import argparse
import json
import math
import pathlib
import subprocess
import struct


def run(command, cwd, input_text=None):
    return subprocess.run(command, cwd=cwd, input=input_text, text=True, encoding="utf-8",
                          capture_output=True, timeout=45, check=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--consumer', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--mcp', required=True)
    parser.add_argument('--directory', required=True)
    args = parser.parse_args()
    directory = pathlib.Path(args.directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    consumer = run([args.consumer, str(directory)], directory)
    print(consumer.stdout, end='')
    assert consumer.returncode == 0, consumer.stderr
    expected = [list(map(float, line.split()))
                for line in (directory / 'expected.tsv').read_text(encoding="utf-8").splitlines()]
    base = dict(path=str(directory / 'graph.pulpgraph'), operation='probe',
                input=str(directory / 'input.wav'), source_node='source', source_port='audio',
                destination_node='downstream', destination_port='audio', block_frames=64)

    def compare(params, code='Attached', rows=None):
        command = [args.cli, 'graph', 'probe', params['path'], '--json']
        for key in ('input', 'source_node', 'source_port', 'destination_node', 'destination_port', 'block_frames'):
            if key in params:
                command += ['--' + key.replace('_', '-'), str(params[key])]
        for key in ('plan', 'latency', 'node_loads'):
            if params.get(key):
                command += ['--' + key.replace('_', '-')]
        cli = run(command, directory)
        assert cli.returncode == (0 if code == 'Attached' else 2), (cli.returncode, cli.stderr, cli.stdout)
        report = json.loads(cli.stdout)
        assert report['code'] == code, report
        request = dict(jsonrpc='2.0', id=1, method='tools/call',
                       params=dict(name='pulp_graph', arguments=params))
        mcp = run([args.mcp], directory, json.dumps(request) + '\n')
        assert mcp.returncode == 0, mcp.stderr
        response = next(json.loads(line) for line in mcp.stdout.splitlines()
                        if json.loads(line).get('id') == 1)['result']
        assert response.get('isError', False) == (code != 'Attached'), response
        mirrored = json.loads(response['content'][0]['text'])
        # Graph lifetime is process-local, not a persisted topology fingerprint.
        if code == 'Attached':
            for result in (report, mirrored):
                assert int(result['identity']['graph_identity']) > 0
                result['identity'].pop('graph_identity')
                assert result['frames'] == 133 and result['sample_rate'] == 48000
                assert result['dropped'] == 0 and result['observation'] == 'source_before_mixing_and_pdc'
                records = result['records']
                assert len(records) == len(rows), ('publication', records, rows)
                for sample, oracle in zip(records, rows):
                    assert sample['edge_index'] == 1, ('selected-edge', sample)
                    actual = [sample[k] for k in ('sequence', 'timestamp_samples', 'frames', 'peak', 'rms')]
                    assert all(math.isclose(a, b, abs_tol=1e-7) for a, b in zip(actual, oracle)), (actual, oracle)
            assert report['identity']['source'] != report['identity']['destination']
        assert mirrored == report, (mirrored, report)

    compare(base, rows=expected)
    introspection = dict(base, plan=True, latency=True)
    compare(introspection, rows=expected)
    report = json.loads(run([args.cli, 'graph', 'probe', introspection['path'], '--json',
                             '--input', introspection['input'], '--source-node', 'source',
                             '--source-port', 'audio', '--destination-node', 'downstream',
                             '--destination-port', 'audio', '--plan', '--latency'], directory).stdout)
    assert report['plan']['connections'][0]['automation'] is False
    assert report['plan']['connections'][0]['audio_rate_modulation'] is False
    assert report['latency_pdc']['nodes'] and report['latency_pdc']['connections']
    cli = run([args.cli, 'graph', 'probe', introspection['path'], '--json',
               '--input', introspection['input'], '--source-node', 'source',
               '--source-port', 'audio', '--destination-node', 'downstream',
               '--destination-port', 'audio', '--node-loads'], directory)
    assert cli.returncode == 0, (cli.returncode, cli.stderr, cli.stdout)
    loads = json.loads(cli.stdout).get('node_loads')
    assert isinstance(loads, list) and loads, loads
    assert all('node_id' in row and 'load' in row and 'callback_count' in row for row in loads), loads
    negative = run([args.cli, 'graph', 'probe', base['path'], '--json', '--input', base['input'],
                    '--source-node', 'source', '--source-port', 'audio',
                    '--destination-node', 'downstream', '--destination-port', 'audio',
                    '--unknown'], directory)
    assert negative.returncode == 2 and json.loads(negative.stdout)['code'] == 'invalid_option', negative.stdout
    outside = run([args.cli, 'graph', 'inspect', base['path'], '--json', '--plan'], directory)
    assert outside.returncode == 2 and json.loads(outside.stdout)['code'] == 'invalid_option', outside.stdout
    request = dict(jsonrpc='2.0', id=2, method='tools/call',
                   params=dict(name='pulp_graph', arguments=dict(path=base['path'],
                                                                  operation='inspect', plan=True)))
    mcp_outside = run([args.mcp], directory, json.dumps(request) + '\n')
    response = next(json.loads(line) for line in mcp_outside.stdout.splitlines()
                    if json.loads(line).get('id') == 2)['result']
    assert response.get('isError') is True and 'only valid for probe' in response['content'][0]['text']
    compare(dict(base, block_frames=128), rows=[[0, 0, 128, .5, .5], [1, 128, 5, .5, .5]])
    compare(dict(base, source_port='absent'), 'UnknownPort')
    compare(dict(base, destination_node='output'), 'MissingEdge')
    compare(dict(base, source_node='notes', destination_node='notes-out'), 'UnsupportedKind')
    compare(dict(base, source_node='absent'), 'unknown_node')
    compare(dict(base, block_frames=0), 'invalid_block_frames')
    compare(dict(base, block_frames=4097), 'invalid_block_frames')
    missing = dict(base)
    del missing['input']
    compare(missing, 'missing_option')
    document = json.loads((directory / 'graph.pulpgraph').read_text(encoding="utf-8"))
    for bad_count in (-1, 2147483647, 1.5):
        oversized = json.loads(json.dumps(document))
        oversized['nodes'][0]['num_output_ports'] = bad_count
        path = directory / 'oversized.pulpgraph'
        path.write_text(json.dumps(oversized), encoding="utf-8")
        compare(dict(base, path=str(path)), 'graph_limit')
    unsupported = json.loads(json.dumps(document))
    unsupported['nodes'][0]['type'] = 'plugin'
    path = directory / 'unsupported.pulpgraph'
    path.write_text(json.dumps(unsupported), encoding="utf-8")
    compare(dict(base, path=str(path)), 'unsupported_node')
    for node in document['nodes']:
        if node['name'] == 'downstream':
            node['name'] = 'source'
    ambiguous = directory / 'ambiguous.pulpgraph'
    ambiguous.write_text(json.dumps(document), encoding="utf-8")
    compare(dict(base, path=str(ambiguous)), 'ambiguous_node')
    malformed = directory / 'malformed.pulpgraph'
    malformed.write_text('{broken', encoding="utf-8")
    compare(dict(base, path=str(malformed)), 'invalid_graph')
    document = json.loads((directory / 'graph.pulpgraph').read_text(encoding="utf-8"))
    for node in document['nodes']:
        if node['name'] == 'source':
            node['gain'] = 3e38
    overflow = directory / 'overflow.pulpgraph'
    overflow.write_text(json.dumps(document), encoding="utf-8")
    def float_wave(name, value):
        data = struct.pack('<133f', *([value] * 133))
        fmt = struct.pack('<HHIIHH', 3, 1, 48000, 192000, 4, 32)
        content = b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
        path = directory / name
        path.write_bytes(b'RIFF' + struct.pack('<I', len(content)) + content)
        return str(path)
    compare(dict(base, path=str(overflow), input=float_wave('overflow.wav', 2.0)), 'non_finite_sample')
    compare(dict(base, input=float_wave('nan.wav', float('nan'))), 'non_finite_input')
    print('graph-edge-probe installed CLI/MCP parity PASS')


if __name__ == '__main__':
    main()
