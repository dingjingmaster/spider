"""Catch step-level parallelism: real crawlers share workers but never overlap themselves."""
import collections
import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
lock = threading.Lock()
active = collections.Counter()
maximum = collections.Counter()
peak = 0

class Site(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        global peak
        crawler = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['crawler'][0]
        with lock:
            active[crawler] += 1
            maximum[crawler] = max(maximum[crawler], active[crawler])
            peak = max(peak, sum(active.values()))
        time.sleep(.2)
        with lock:
            active[crawler] -= 1
        payload = b'{"ok":true}'
        self.send_response(200)
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass

def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Site)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    def api(path, method='GET', value=None, expected=200):
        request = urllib.request.Request(base + path, method=method,
            data=None if value is None else json.dumps(value).encode(),
            headers={'Content-Type': 'application/json', 'X-Spider-Request': '1'})
        try:
            response = opener.open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            assert response.status == expected, (path, response.status, data)
            return json.loads(data)
    def rule(name):
        return {'schema_version': 2, 'name': name, 'entry': 'each',
            'vars': {'rows': [1, 2, 3, 4]}, 'defaults': {'delay_ms': 0},
            'steps': [
                {'id': 'each', 'type': 'foreach', 'items': '{{run.rows}}', 'body': 'fetch'},
                {'id': 'fetch', 'type': 'request', 'request': {
                    'url': f'http://127.0.0.1:{server.server_port}/probe?crawler={name}'},
                    'next': 'save'},
                {'id': 'save', 'type': 'emit', 'fields': {'number': '{{input}}'}}]}
    with tempfile.TemporaryDirectory(prefix='spider-scheduler-') as directory, tempfile.TemporaryFile(mode='w+') as log:
        env = {**os.environ, 'NO_PROXY': '127.0.0.1,localhost', 'no_proxy': '127.0.0.1,localhost'}
        process = subprocess.Popen([binary, '--port', str(port), '--workers', '4',
            '--data-dir', directory, '--ui-dir', str(pathlib.Path(__file__).resolve().parents[1] / 'web/ui')],
            stdout=subprocess.DEVNULL, stderr=log, env=env)
        try:
            for _ in range(150):
                assert process.poll() is None, 'service exited'
                try:
                    api('/api/health')
                    break
                except OSError:
                    time.sleep(.03)
            runs = []
            for name in ('A', 'B'):
                spider = api('/api/spiders', 'POST', rule(name), 201)['id']
                runs.append(api(f'/api/spiders/{spider}/run', 'POST', {}, 201)['id'])
                api(f'/api/spiders/{spider}/run', 'POST', {}, 409)
            for run in runs:
                for _ in range(200):
                    row = api(f'/api/runs/{run}')
                    if row['status'] not in ('queued', 'running'):
                        break
                    time.sleep(.025)
                assert row['status'] == 'succeeded', row
                assert len(api(f'/api/runs/{run}/items')) == 4
            assert maximum['A'] == maximum['B'] == 1, f'same crawler overlapped: {dict(maximum)}'
            assert peak >= 2, 'different crawlers did not execute concurrently'
            process.terminate()
            assert process.wait(timeout=10) == 0
            print('scheduler integration passed: same crawler serial; different crawlers concurrent')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            log.seek(0)
            output = log.read()
            if sys.exc_info()[0]:
                print(output[-6000:], file=sys.stderr)
            assert 'AddressSanitizer' not in output and 'runtime error:' not in output, output
    server.shutdown()

if __name__ == '__main__':
    main()
