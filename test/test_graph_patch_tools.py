#!/usr/bin/env python3
"""Public installed API / CLI / MCP diagnostics and notation parity."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def run(args, *, data=None, cwd=None):
    return subprocess.run([str(a) for a in args], input=data, text=True, encoding="utf-8",
                          capture_output=True, timeout=30, cwd=cwd)


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

        # Diff: the MCP operation is the CLI's `graph diff --json` on the same
        # inputs. Every case compares the MCP text to the CLI JSON and maps the
        # CLI exit code to isError (a comparison with differences is not one).
        def mcp_diff(arguments, ident):
            request = {'jsonrpc': '2.0', 'id': ident, 'method': 'tools/call',
                       'params': {'name': 'pulp_graph',
                                  'arguments': dict(arguments, operation='diff')}}
            wire = json.dumps(request)
            mcp = run([args.mcp], data=f'Content-Length: {len(wire)}\r\n\r\n{wire}', cwd=args.root)
            payload = json.loads(mcp.stdout.split('\n\n', 1)[1])['result']
            return payload, json.loads(payload['content'][0]['text'])

        def check_diff(before, after, expect_rc, ident):
            cli = run([args.cli, 'graph', 'diff', before, after, '--json'])
            assert cli.returncode == expect_rc, (before, after, cli)
            payload, text = mcp_diff({'before': str(before), 'after': str(after)}, ident)
            assert text == json.loads(cli.stdout), (text, cli.stdout)
            assert payload.get('isError', False) == (expect_rc != 0), payload
            return text

        def digest(path):
            return hashlib.sha256(Path(path).read_bytes()).hexdigest()

        base = directory / 'diff-base.pat'
        base.write_text(good, encoding='utf-8')
        trimmed = directory / 'diff-trim.pat'
        trimmed.write_text(good.replace('0.25', '0.5'), encoding='utf-8')
        before_doc = directory / 'diff-before.pulpgraph'
        after_doc = directory / 'diff-after.pulpgraph'
        for source, target in ((base, before_doc), (trimmed, after_doc)):
            assert run([args.cli, 'graph', 'save', source, '--output', target,
                        '--saved-identity']).returncode == 0
        plain_doc = directory / 'diff-plain.pulpgraph'
        assert run([args.cli, 'graph', 'save', base, '--output', plain_doc]).returncode == 0
        watched = [before_doc, after_doc, plain_doc]
        digests = [digest(path) for path in watched]

        # Semantic change, with before/after attribution that flips with the
        # argument order.
        forward = check_diff(before_doc, after_doc, 0, 200)
        assert forward['equal'] is False
        assert forward['nodes']['changed'][0]['fields'] == ['gain']
        backward = check_diff(after_doc, before_doc, 0, 201)
        assert backward['before_digest'] == forward['after_digest']
        assert backward['after_digest'] == forward['before_digest']
        assert forward['before_digest'] != forward['after_digest']

        # Control: identical documents compare equal and succeed.
        assert check_diff(before_doc, before_doc, 0, 202)['equal'] is True

        # Layout alone is reported but is not a semantic difference.
        moved = json.loads(before_doc.read_text(encoding='utf-8'))
        moved['nodes'][0]['layout'] = {'x': 3.0, 'y': 4.0}
        moved_doc = directory / 'diff-moved.pulpgraph'
        moved_doc.write_text(json.dumps(moved), encoding='utf-8')
        layout = check_diff(before_doc, moved_doc, 0, 203)
        assert layout['equal'] is True and len(layout['layout_changed']) == 1, layout
        assert layout['nodes']['changed'] == []

        # Refusals: the missing envelope on each side, attributed to the side.
        refused_before = check_diff(plain_doc, after_doc, 1, 204)
        assert refused_before['code'] == 'InvalidDocument'
        assert refused_before['side'] == 'before'
        assert refused_before['refusal'] in ('logical_identity_missing', 'invalid_format_version'), refused_before
        refused_after = check_diff(before_doc, plain_doc, 1, 205)
        assert refused_after['side'] == 'after'

        # Malformed identities and documents.
        broken_identity = json.loads(before_doc.read_text(encoding='utf-8'))
        broken_identity['logical_identities'] = {'bogus': 'not-an-identity'}
        broken_doc = directory / 'diff-broken-identity.pulpgraph'
        broken_doc.write_text(json.dumps(broken_identity), encoding='utf-8')
        malformed = check_diff(broken_doc, after_doc, 1, 206)
        assert malformed['side'] == 'before' and malformed['code'] == 'InvalidDocument'
        not_json = directory / 'diff-not-json.pulpgraph'
        not_json.write_text('{broken', encoding='utf-8')
        assert check_diff(before_doc, not_json, 1, 207)['side'] == 'after'

        # Missing and oversized inputs, each side.
        absent = directory / 'diff-absent.pulpgraph'
        assert check_diff(absent, after_doc, 1, 208)['code'] == 'FileUnreadable'
        assert check_diff(before_doc, absent, 1, 209)['code'] == 'FileUnreadable'
        oversized = directory / 'diff-oversized.pulpgraph'
        with open(oversized, 'wb') as handle:
            handle.truncate(64 * 1024 * 1024 + 1)
        assert check_diff(oversized, after_doc, 1, 210)['code'] == 'ResourceLimit'
        assert check_diff(before_doc, oversized, 1, 211)['code'] == 'ResourceLimit'

        # Input mistakes the CLI refuses with exit 2 are MCP errors here.
        payload, text = mcp_diff({'before': str(before_doc)}, 212)
        assert payload['isError'] and text['code'] == 'missing_input'
        cli = run([args.cli, 'graph', 'diff', before_doc, '--json'])
        assert cli.returncode == 2 and json.loads(cli.stdout) == text
        payload, text = mcp_diff({'path': str(before_doc), 'before': str(before_doc),
                                  'after': str(after_doc)}, 213)
        assert payload['isError'] and text['code'] == 'invalid_option'

        # No operation above modified any input.
        assert [digest(path) for path in watched] == digests

    print('PASS API/CLI/MCP 7 diagnostic controls, catalog without path, notation/save roundtrip and diff parity')


if __name__ == '__main__':
    main()
