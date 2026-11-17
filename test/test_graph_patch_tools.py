#!/usr/bin/env python3
"""Public installed API / CLI / MCP diagnostics and notation parity."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run(args, *, data=None, cwd=None):
    return subprocess.run([str(a) for a in args], input=data, text=True, encoding="utf-8",
                          capture_output=True, timeout=30, cwd=cwd)


# Spectral analysis -> identity stage -> synthesis between mono boundaries.
FLOW2 = ('# flow 2\n'
         'analysis : pulp/spectral_analysis (fft 256, hop 64, channels 1, rate 48000)\n'
         'in : pulp/audio_in (channels 1)\n'
         'out : pulp/audio_out (channels 1)\n'
         'pass : pulp/spectral_stage (fft 256, hop 64, channels 1, rate 48000, kind identity)\n'
         'synthesis : pulp/spectral_synthesis (fft 256, hop 64, channels 1, rate 48000)\n'
         'analysis.frames >> pass.frames_in\n'
         'in.audio >> analysis.audio\n'
         'pass.frames_out >> synthesis.frames\n'
         'synthesis.audio >> out.audio\n')


def mcp_call(args, request_id, arguments):
    request = {'jsonrpc': '2.0', 'id': request_id, 'method': 'tools/call',
               'params': {'name': 'pulp_graph', 'arguments': arguments}}
    wire = json.dumps(request)
    mcp = run([args.mcp], data=f'Content-Length: {len(wire)}\r\n\r\n{wire}', cwd=args.root)
    return json.loads(mcp.stdout.split('\n\n', 1)[1])['result']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--consumer', type=Path, required=True)
    p.add_argument('--cli', type=Path, required=True)
    p.add_argument('--mcp', type=Path, required=True)
    p.add_argument('--root', type=Path, required=True)
    args = p.parse_args()
    good = '# flow 1\nin : pulp/audio_in\ngain : pulp/gain\nout : pulp/audio_out\nin.audio >> gain.audio\ngain.audio >> out.audio\ngain.gain = 0.25\n'
    fixtures = [
        (good, 'Ok'),
        (good.replace('in.audio >>', 'in.typo >>'), 'UnknownPort'),
        (good.replace('in : pulp/audio_in', 'in : pulp/midi_in'), 'KindMismatch'),
        (good + 'gain.audio >> gain.audio\n', 'IllegalCycle'),
        (good.replace('0.25', '1e999'), 'ParameterOutOfRange'),
        (good.replace('gain.audio >> out.audio', 'absent.audio >> out.audio'), 'UnknownNode'),
        (FLOW2, 'Ok'),
        (FLOW2.replace(', rate 48000)\nin', ')\nin'), 'SyntaxError'),
        (FLOW2.replace('channels 1, rate 48000)\nanalysis.frames', 'channels 1, rate 44100)\nanalysis.frames'),
         'SampleRateMismatch'),
        (FLOW2.replace('in : pulp/audio_in (channels 1)', 'in : pulp/audio_in (channels 17)'),
         'UnsupportedChannelLayout'),
    ]
    with tempfile.TemporaryDirectory(prefix='graph-patch-tools-') as temporary:
        directory = Path(temporary)
        for i, (text, code) in enumerate(fixtures):
            path = directory / f'{i}.pat'
            path.write_text(text, encoding="utf-8")
            api = run([args.consumer, '--validate', path])
            cli = run([args.cli, 'graph', 'validate', path, '--json'])
            api_json, cli_json = json.loads(api.stdout), json.loads(cli.stdout)
            assert api_json['code'] == cli_json['code'] == code, (api, cli)
            assert api.returncode == cli.returncode == (0 if code == 'Ok' else 1)
            # Compare every field on failures; CLI success adds graph counts.
            if code != 'Ok':
                assert api_json == cli_json, (api_json, cli_json)
                assert api_json['suggested_fix'] and api_json['line']
            request = {'jsonrpc': '2.0', 'id': i+1, 'method': 'tools/call',
                       'params': {'name': 'pulp_graph', 'arguments': {'path': str(path), 'operation': 'validate'}}}
            wire = json.dumps(request)
            mcp = run([args.mcp], data=f'Content-Length: {len(wire)}\r\n\r\n{wire}', cwd=args.root)
            payload = json.loads(mcp.stdout.split('\n\n', 1)[1])['result']
            assert json.loads(payload['content'][0]['text']) == cli_json, (payload, cli_json)
            assert payload.get('isError', False) == (code != 'Ok')
        path = directory / '0.pat'
        first = run([args.cli, 'graph', 'fmt', path, '--json'])
        canonical = json.loads(first.stdout)['notation']
        path.write_text(canonical, encoding="utf-8")
        assert json.loads(run([args.cli, 'graph', 'fmt', path, '--json']).stdout)['notation'] == canonical
        saved = directory / 'saved.pulpgraph'
        assert run([args.cli, 'graph', 'save', path, '--output', saved]).returncode == 0
        assert json.loads(run([args.cli, 'graph', 'fmt', saved, '--notation', '--json']).stdout)['notation'] == canonical
        damaged = json.loads(saved.read_text(encoding="utf-8"))
        damaged['connections'][0]['source_port'] = 999
        broken = directory / 'dangling.pulpgraph'
        broken.write_text(json.dumps(damaged), encoding="utf-8")
        api = run([args.consumer, '--validate', broken])
        cli = run([args.cli, 'graph', 'validate', broken, '--json'])
        assert api.returncode == cli.returncode == 1
        assert json.loads(api.stdout) == json.loads(cli.stdout)
        assert json.loads(cli.stdout)['code'] == 'UnknownPort'
        request = {'jsonrpc': '2.0', 'id': 100, 'method': 'tools/call',
                   'params': {'name': 'pulp_graph', 'arguments': {'path': str(broken), 'operation': 'validate'}}}
        wire = json.dumps(request)
        mcp = run([args.mcp], data=f'Content-Length: {len(wire)}\r\n\r\n{wire}', cwd=args.root)
        payload = json.loads(mcp.stdout.split('\n\n', 1)[1])['result']
        assert payload['isError'] and json.loads(payload['content'][0]['text']) == json.loads(cli.stdout)
        request['id'] = 101
        request['params']['arguments'] = {'operation': 'catalog'}
        wire = json.dumps(request)
        mcp = run([args.mcp], data=f'Content-Length: {len(wire)}\r\n\r\n{wire}', cwd=args.root)
        payload = json.loads(mcp.stdout.split('\n\n', 1)[1])['result']
        assert not payload.get('isError', False)
        assert json.loads(payload['content'][0]['text']) == json.loads(run([args.cli, 'graph', 'catalog', '--json']).stdout)

        # Flow 2: CLI and MCP fmt print the same canonical text, which is the
        # input, and a refused document carries the same diagnostic.
        flow2 = directory / 'flow2.pat'
        flow2.write_text(FLOW2, encoding='utf-8')
        cli = run([args.cli, 'graph', 'fmt', flow2, '--json'])
        assert cli.returncode == 0, cli
        cli_json = json.loads(cli.stdout)
        assert cli_json['notation'] == FLOW2, cli_json
        payload = mcp_call(args, 200, {'path': str(flow2), 'operation': 'fmt'})
        assert not payload.get('isError', False), payload
        assert json.loads(payload['content'][0]['text']) == cli_json
        mismatch = directory / 'flow2-mismatch.pat'
        mismatch.write_text(FLOW2.replace('rate 48000)\nanalysis.frames', 'rate 44100)\nanalysis.frames'),
                            encoding='utf-8')
        cli = run([args.cli, 'graph', 'fmt', mismatch, '--json'])
        assert cli.returncode == 1 and json.loads(cli.stdout)['code'] == 'SampleRateMismatch', cli
        payload = mcp_call(args, 201, {'path': str(mismatch), 'operation': 'fmt'})
        assert payload['isError'] and json.loads(payload['content'][0]['text']) == json.loads(cli.stdout)

    print('PASS API/CLI/MCP 11 diagnostic controls, catalog without path, notation/save roundtrip '
          'and flow 2 fmt parity')


if __name__ == '__main__':
    main()
