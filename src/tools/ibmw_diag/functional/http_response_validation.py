#!/usr/bin/env python3
"""Reject malformed RPC wire responses through the real HTTP driver and CLI."""
import argparse
import http.server
import json
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True)
    args = parser.parse_args()
    received = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            received.append((self.path, self.rfile.read(int(self.headers['Content-Length']))))
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(self.server.response)))
            self.end_headers()
            self.wfile.write(self.server.response)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    cases = [
        (['param', 'set', 'CarModule', 'key', 'value'], None, None, 'Set parameter successful\n', 'Set'),
        (['param', 'get', 'CarModule', 'key', '--hide-type'], 'parameter_value', '', '\n', 'Get'),
        (['param', 'list', 'CarModule'], 'parameter_keys', [], '', 'List'),
        (['param', 'list'], 'module_names', [], '', 'ListModules'),
        (['log', 'set', 'CarModule', 'INFO'], None, None, 'Set log level successful\n', 'SetModuleLogLevel'),
        (['log', 'list'], 'module_log_level_map', {}, '', 'GetModuleLogLevel'),
    ]
    count = 0
    try:
        with tempfile.TemporaryDirectory(prefix='ibmw-wire-response-') as cwd:
            for command, field, empty, output, method in cases:
                valid = dict(code=0, msg='')
                if field:
                    valid[field] = empty
                variants = [(valid, True), ({'code': 2, 'msg': 'REMOTE_ERROR'}, False)]
                variants += [(x, False) for x in [{}, None, [], 0, 'text',
                    {k: v for k, v in valid.items() if k != 'code'},
                    {k: v for k, v in valid.items() if k != 'msg'}]]
                variants += [({**valid, 'code': code}, False) for code in [-1, 0.0, 0.5, True, '0', None]]
                variants.append(({**valid, 'msg': 0}, False))
                if method != 'Set':
                    variants.append(({**valid, 'code': 2**32}, False))
                if field:
                    variants += [({'code': 0, 'msg': ''}, False),
                                 ({**valid, field: None}, False), ({**valid, field: 42}, False)]
                    if isinstance(empty, (list, dict)):
                        variants.append(({**valid, field: [42]}, False))
                    if isinstance(empty, dict):
                        variants.append(({**valid, field: [['core', 'INFO']]}, False))
                for body, success in variants:
                    server.response = json.dumps(body).encode()
                    before = len(received)
                    result = subprocess.run([args.cli, *command, '--http',
                        f'127.0.0.1:{server.server_port}', '--service', 'wire-validation', '--timeout', '1'],
                        capture_output=True, text=True, cwd=cwd, timeout=5)
                    detail = (command, body, result.returncode, result.stdout, result.stderr)
                    assert len(received) == before + 1 and received[-1][0].endswith('/' + method), (received[-1:], detail)
                    assert (result.returncode == 0) == success, detail
                    if success:
                        assert result.stdout == output, detail
                    else:
                        assert not result.stdout and result.stderr, detail
                    count += 1
        print(f'HTTP wire response validation: {count} cases passed across six CLI routes')
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


if __name__ == '__main__':
    main()
