#!/usr/bin/env python3
"""Actual provider/CLI deadline, unload and helper ownership regressions."""
import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import threading
import time


@contextlib.contextmanager
def directory_server(ipv6=False, delay=0):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            assert self.path == '/ibmw/discovery/services/v1'
            time.sleep(delay)
            payload = json.dumps(dict(version=1, instance='deadline-test', services=[dict(
                name='dns-regression', request_type='json:request', response_type='json:response')])).encode()
            try:
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

    class Server(http.server.ThreadingHTTPServer):
        address_family = socket.AF_INET6 if ipv6 else socket.AF_INET
        daemon_threads = True

    with Server(('::1' if ipv6 else '127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            yield server.server_port
        finally:
            server.shutdown()
            thread.join()


@contextlib.contextmanager
def ready_registry(directory):
    registry = Path(directory) / 'v2'
    registry.mkdir(mode=0o700)
    address = registry / 'ready.sock'
    with socket.socket(socket.AF_UNIX) as control:
        control.bind(str(address))
        address.chmod(0o600)
        start_ticks = int(Path('/proc/self/stat').read_text().rsplit(') ', 1)[1].split()[19])
        record = dict(protocol_version=2, runtime_id='ready', pid=os.getpid(),
            process_start_time_ticks=start_ticks, control_socket=str(address), topics=[], services=[dict(
                endpoint_id='ready-service', service_name='registry-regression', request_type='json:request',
                response_type='json:response', module_name='test', package_path='', available_drivers=['http'],
                server=True, hidden=False)])
        registration = registry / 'ready.json'
        registration.write_text(json.dumps(record))
        registration.chmod(0o600)
        yield


def main():
    parser = argparse.ArgumentParser()
    for arg in ['cli', 'probe', 'fault']:
        parser.add_argument('--' + arg, required=True)
    args = parser.parse_args()
    plugin_dir = str(Path(args.cli).resolve().parent)
    env = {**os.environ, 'LD_PRELOAD': args.fault, 'IBMW_DISCOVERY_PLUGIN_DIR': plugin_dir}
    measurements = {}
    with tempfile.TemporaryDirectory(prefix='ibmw-http-deadline-') as directory, ready_registry(directory):
        def probe(label, endpoint, timeout, count, success):
            result = subprocess.run([args.probe, plugin_dir, directory, endpoint,
                str(timeout), str(count), 'success' if success else 'timeout'],
                capture_output=True, text=True, env=env, timeout=20, cwd=directory)
            assert result.returncode == 0, (label, result.stdout, result.stderr)
            value = json.loads(result.stdout)
            if not success:
                assert max(value['elapsed_ms']) < timeout + 150, (label, value)
            measurements[label] = value

        probe('slow_dns_repeated', 'slow-pr12.invalid:12345', 100, 12, False)
        for combined in [False, True]:
            start = time.monotonic()
            result = subprocess.run([args.cli, 'service', 'list', '--http', 'slow-pr12.invalid:12345',
                '--timeout', '0.1', *(['--registry', '--registry-path', directory] if combined else [])],
                capture_output=True, text=True, env=env, timeout=3, cwd=directory)
            elapsed = time.monotonic() - start
            assert result.returncode != 0 and 'timed out' in result.stderr, result
            assert result.stdout == ('registry-regression\n' if combined else ''), result
            assert elapsed < .75, (elapsed, result)
            measurements['cli_combined' if combined else 'cli_http'] = elapsed * 1000
        with directory_server() as port:
            probe('ipv4', f'127.0.0.1:{port}', 1000, 20, True)
            probe('hostname', f'localhost:{port}', 1000, 20, True)
            # A relocated provider must use its own helper, never PATH or the
            # build-tree executable. Numeric addresses work without the helper.
            isolated = Path(directory) / 'provider'
            isolated.mkdir()
            shutil.copy2(Path(plugin_dir) / 'libibmw_service_discovery_http.so', isolated)
            isolated_env = {**env, 'IBMW_DISCOVERY_PLUGIN_DIR': str(isolated), 'PATH': '/nonexistent'}
            def isolated_query(host):
                return subprocess.run([args.cli, 'service', 'list', '--http', f'{host}:{port}',
                    '--timeout', '1'], env=isolated_env, capture_output=True, text=True, timeout=3, cwd=directory)
            result = isolated_query('localhost')
            assert result.returncode != 0 and 'DNS helper spawn' in result.stderr, result
            result = isolated_query('127.0.0.1')
            assert result.returncode == 0 and result.stdout == 'dns-regression\n', result
            helper = isolated / 'ibmw_service_dns_helper'
            shutil.copy2(Path(plugin_dir) / helper.name, helper)
            helper.chmod(0o600)
            result = isolated_query('localhost')
            assert result.returncode != 0 and 'DNS helper spawn' in result.stderr, result
            helper.chmod(0o700)
            result = isolated_query('localhost')
            assert result.returncode == 0 and result.stdout == 'dns-regression\n', result
            measurements['helper_lookup_cases'] = 4
        with directory_server(ipv6=True) as port:
            probe('ipv6', f'[::1]:{port}', 1000, 3, True)
        with directory_server(delay=.4) as port:
            probe('dns_and_read_shared_budget', f'budget-pr12.invalid:{port}', 500, 3, False)
            probe('numeric_read_timeout', f'127.0.0.1:{port}', 100, 3, False)
        # A full localhost listen queue deterministically leaves connect pending.
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(0)
            with socket.create_connection(listener.getsockname(), timeout=1):
                probe('connect_timeout', f'127.0.0.1:{listener.getsockname()[1]}', 100, 3, False)
    print(json.dumps(measurements, sort_keys=True))


if __name__ == '__main__':
    main()
