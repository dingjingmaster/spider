"""Transient preview sessions, real HTTP continuity, redaction and no persistence."""
import concurrent.futures
import http.server
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
TOKEN = "preview-secret-token"
COOKIE = "preview-secret-cookie"


class Fixture(http.server.BaseHTTPRequestHandler):
    calls = []
    slow_started = threading.Event()

    def do_GET(self):
        Fixture.calls.append((self.path, dict(self.headers)))
        if self.path.startswith('/slow'):
            Fixture.slow_started.set()
            time.sleep(1)
        if self.path.startswith('/fail'):
            self.send_response(503)
            self.send_header('Set-Cookie', 'sid=broken-cookie; Path=/')
            self.end_headers()
            return
        if self.path.startswith('/protected'):
            if self.headers.get('Authorization') != 'Bearer ' + TOKEN or ('sid=' + COOKIE) not in self.headers.get('Cookie', ''):
                self.send_response(403)
                self.end_headers()
                return
        body = json.dumps({'token': TOKEN, 'public': 'ok', 'private_code': 'custom-secret', 'session_id': 'data-session-secret', 'nested': {'Password': 'nested-secret'}}).encode()
        self.send_response(200)
        self.send_header('Set-Cookie', 'sid=' + COOKIE + '; Path=/')
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:' + str(port())
    target = 'http://127.0.0.1:' + str(fixture.server_port)
    rule = {'schema_version': 2, 'name': 'Transient preview', 'entry': 'login', 'defaults': {'delay_ms': 0, 'retries': 0}, 'sensitive_fields': ['private_code', 'session_id', 'next_steps'], 'steps': [
        {'id': 'login', 'type': 'request', 'request': {'url': target + '/login?api_key=url-secret', 'headers': {'X-Api-Key': 'header-secret'}}, 'response_json': True, 'next': 'private'},
        {'id': 'private', 'type': 'request', 'request': {'url': target + '/protected', 'headers': {'Authorization': 'Bearer {{steps.login.response.json.token}}'}}, 'response_json': True, 'next': 'save'},
        {'id': 'save', 'type': 'emit', 'fields': {'token': '{{steps.login.response.json.token}}', 'visible': '{{steps.private.response.json.public}}'}},
        {'id': 'broken', 'type': 'request', 'input': {'changed': True}, 'request': {'url': target + '/fail'}},
        {'id': 'schema', 'type': 'request', 'input': {'id': 'wrong'}, 'input_schema': {'type': 'object', 'properties': {'id': {'type': 'integer'}}}, 'request': {'url': target + '/schema'}},
        {'id': 'loop', 'type': 'foreach', 'items': [{'id': 1}, {'id': 2}], 'body': 'inspect'},
        {'id': 'inspect', 'type': 'join', 'fields': {'selected': '{{input.id}}'}},
        {'id': 'fork', 'type': 'parallel', 'branches': {'left': 'inspect', 'right': 'save'}},
        {'id': 'condition', 'type': 'condition', 'test': {'left': '{{input.id}}', 'op': 'eq', 'right': 2}, 'then': 'inspect', 'else': 'save'},
        {'id': 'transform', 'type': 'transform', 'value': '  hello ', 'operations': [{'op': 'trim'}]},
        {'id': 'file', 'type': 'download', 'url': target + '/file'},
        {'id': 'slow', 'type': 'request', 'request': {'url': target + '/slow'}},
        {'id': 'short', 'type': 'request', 'request': {'url': target + '/short'}, 'response_json': True, 'response_body': False, 'retain_steps': []},
        {'id': 'token_value', 'type': 'transform', 'value': '{{steps.login.response.json.token}}', 'operations': [{'op': 'trim'}], 'retain_steps': ['token_value']},
        {'id': 'token_emit', 'type': 'emit', 'fields': {'value': '{{steps.token_value}}'}},
        {'id': 'token_use', 'type': 'request', 'request': {'url': target + '/protected', 'headers': {'Authorization': 'Bearer {{steps.token_value}}', 'X-Token': '{{steps.token_value}}'}}},
        {'id': 'header_source', 'type': 'request', 'request': {'url': target + '/login', 'headers': {'X-Token': '{{input.raw}}'}}},
        {'id': 'raw_value', 'type': 'transform', 'value': '{{input.raw}}', 'operations': [{'op': 'trim'}], 'retain_steps': ['raw_value']},
        {'id': 'projection', 'type': 'join', 'input': {'value': {'$ref': '/input/token'}}, 'fields': {'value': '{{input.value}}'}},
        {'id': 'source_trim', 'type': 'transform', 'value': '{{input.token}}', 'operations': [{'op': 'trim'}], 'retain_steps': ['source_trim']},
        {'id': 'source_cast', 'type': 'transform', 'value': '{{input.token}}', 'operations': [{'op': 'trim'}, {'op': 'cast', 'type': 'integer'}], 'retain_steps': ['source_cast']},
        {'id': 'cast_check', 'type': 'condition', 'test': {'left': '{{steps.source_cast}}', 'op': 'eq', 'right': 56789}, 'then': 'inspect', 'else': 'file'},
        {'id': 'source_bool', 'type': 'transform', 'value': '{{input.token}}', 'operations': [{'op': 'cast', 'type': 'boolean'}]},
        {'id': 'pipeline_ref', 'type': 'transform', 'value': [{}], 'operations': [{'op': 'map', 'fields': {'x': {'$ref': '/steps/login/token'}}}, {'op': 'pick', 'path': '/0/x'}, {'op': 'trim'}], 'retain_steps': ['pipeline_ref']},
        {'id': 'pipeline_mixed', 'type': 'transform', 'value': [{'id': 7}], 'operations': [{'op': 'map', 'fields': {'id': {'$ref': '/input/id'}, 'x': {'$ref': '/steps/login/token'}}}, {'op': 'pick', 'path': '/0/x'}, {'op': 'trim'}], 'retain_steps': ['pipeline_mixed']},
        {'id': 'pipeline_concat', 'type': 'transform', 'value': [{'id': 7}], 'operations': [{'op': 'map', 'fields': {'x': '{{input.id}}:{{steps.login.token}}'}}, {'op': 'pick', 'path': '/0/x'}, {'op': 'replace', 'old': '7:', 'new': ''}, {'op': 'trim'}], 'retain_steps': ['pipeline_concat']},
        {'id': 'pipeline_cast', 'type': 'transform', 'value': [{}], 'operations': [{'op': 'map', 'fields': {'x': {'$ref': '/steps/login/token'}}}, {'op': 'pick', 'path': '/0/x'}, {'op': 'trim'}, {'op': 'cast', 'type': 'integer'}], 'retain_steps': ['pipeline_cast']}
    ]}

    def api(path, data=None, error=False):
        request = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(), headers={'X-Spider-Request': '1', 'Content-Type': 'application/json'})
        try:
            response = OPENER.open(request, timeout=10)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            body = response.read()
            assert (response.status >= 400) if error else response.status == 200, (response.status, body)
            return json.loads(body)

    sessions = set()

    def preview(step, session=None, error=False, **kwargs):
        payload = {'rule': rule, 'step_id': step, **kwargs}
        if session:
            payload['session_id'] = session
        result = api('/api/preview/step', payload, error)
        if not error:
            sessions.add(result['session_id'])
        return result

    with tempfile.TemporaryDirectory(prefix='spider-preview-') as data, tempfile.TemporaryFile(mode='w+') as log:
        command = [binary, '--port', base.rsplit(':', 1)[1], '--data-dir', data, '--ui-dir', str(pathlib.Path(__file__).resolve().parents[1] / 'web/ui')]
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log, env={**os.environ, 'NO_PROXY': 'localhost,127.0.0.1', 'no_proxy': 'localhost,127.0.0.1'})
        try:
            for _ in range(100):
                assert proc.poll() is None, 'service exited'
                try:
                    api('/api/health')
                    break
                except OSError:
                    time.sleep(.03)
            # RED: the old binary has no continuous preview endpoint.
            login = preview('login')
            sid = login['session_id']
            assert len(sid) == 64 and all(c in '0123456789abcdef' for c in sid)
            assert login['next_steps'] == ['private']
            assert login['output']['iteration'] == 1
            serialized = json.dumps(login)
            for secret in (TOKEN, COOKIE, 'custom-secret', 'nested-secret', 'url-secret', 'header-secret', 'data-session-secret'):
                assert secret not in serialized, (secret, serialized)
            assert login['output']['response']['json']['public'] == 'ok'
            assert 'REDACTED' in serialized
            scalar_session = preview('login')['session_id']
            derived = preview('token_value', scalar_session)
            assert derived['output'] == '[REDACTED]' and TOKEN not in json.dumps(derived)
            assert '_debug_secrets' not in json.dumps(derived)
            emitted = preview('token_emit', scalar_session)
            assert emitted['output']['value'] == '[REDACTED]' and TOKEN not in json.dumps(emitted)
            used = preview('token_use', scalar_session)
            assert used['output']['response']['status'] == 200 and TOKEN not in json.dumps(used)
            assert used['request']['headers']['X-Token'] == '[REDACTED]'
            projected = preview('projection', context={'input': {'token': 'projection-secret'}})
            assert projected['output']['value'] == '[REDACTED]' and 'projection-secret' not in json.dumps(projected)
            trimmed = preview('source_trim', context={'input': {'token': '  trim-secret  '}})
            assert trimmed['output'] == '[REDACTED]' and 'trim-secret' not in json.dumps(trimmed)
            cast = preview('source_cast', context={'input': {'token': ' 56789 '}})
            assert cast['output'] == '[REDACTED]' and '56789' not in json.dumps(cast)
            assert preview('cast_check', cast['session_id'])['next_steps'] == ['inspect']
            cast_bool = preview('source_bool', context={'input': {'token': 'true'}})
            assert cast_bool['output'] == '[REDACTED]'
            for pipeline, secret in [('pipeline_ref', 'pipeline-sensitive-value'), ('pipeline_mixed', 'mixed-sensitive-value'), ('pipeline_concat', 'concat-sensitive-value'), ('pipeline_cast', '67890')]:
                result = preview(pipeline, context={'steps': {'login': {'token': '  ' + secret + '  '}}})
                assert result['output'] == '[REDACTED]' and secret not in json.dumps(result), (pipeline, result)
                assert result['context']['steps'][pipeline] == '[REDACTED]'
            header_session = preview('header_source', context={'input': {'raw': 'header-derived-secret'}})['session_id']
            header_derived = preview('raw_value', header_session)
            assert header_derived['output'] == '[REDACTED]' and 'header-derived-secret' not in json.dumps(header_derived)
            # A one-character known secret must not expand a bounded value without limit.
            expanded = preview('raw_value', context={'input': {'token': 'a', 'raw': 'a' * 450000}})
            assert len(json.dumps(expanded)) < 1048576, 'redaction amplified the bounded preview value'
            private = preview('private', sid)
            assert private['output']['response']['status'] == 200  # raw token + Cookie survived server-side
            assert TOKEN not in json.dumps(private)
            preview('broken', sid, error=True)
            assert preview('private', sid)['output']['response']['status'] == 200  # failed Cookie update rolled back
            assert preview('save', sid)['output']['visible'] == 'ok'
            before = len(Fixture.calls)
            dry = preview('login', sid, dry_run=True)
            assert len(Fixture.calls) == before
            assert dry['request']['url'].endswith('api_key=[REDACTED]') or 'url-secret' not in dry['request']['url']
            assert dry['context']['steps']['login']['iteration'] == 1
            assert preview('login', sid)['output']['iteration'] == 2
            preview('schema', sid, error=True)
            assert not any(path == '/schema' for path, _ in Fixture.calls)
            before_fail = preview('inspect', context={'input': {'id': 19}})
            fail_sid = before_fail['session_id']
            preview('broken', fail_sid, error=True)
            assert preview('inspect', fail_sid)['output']['selected'] == 19
            preview('inspect', fail_sid, context={}, error=True)
            preview('inspect', '0' * 64, error=True)
            changed = dict(rule, name='Changed rule')
            api('/api/preview/step', {'rule': changed, 'step_id': 'login', 'session_id': sid}, error=True)
            preview('file', sid, error=True)
            selected = preview('loop', sid, item_index=1)
            assert selected['context']['input'] == {'id': 2} and selected['next_steps'] == ['inspect']
            assert preview('inspect', sid)['output']['selected'] == 2
            default = preview('loop', sid)
            assert default['context']['input'] == {'id': 1} and default['notice']
            preview('loop', sid, item_index=9, error=True)
            assert preview('inspect', sid)['output']['selected'] == 1
            assert preview('fork', sid, branch='left')['next_steps'] == ['inspect']
            preview('fork', sid, error=True)
            preview('fork', sid, branch='unknown', error=True)
            assert preview('condition', sid)['next_steps'] == ['save']
            assert preview('transform', sid)['output'] == 'hello'
            short = preview('short')
            assert short['context']['steps'] == {} and 'body' not in short['output']['response']
            assert preview('short', short['session_id'])['output']['iteration'] == 2
            with concurrent.futures.ThreadPoolExecutor() as pool:
                iterations = list(pool.map(lambda _: preview('short', short['session_id'])['output']['iteration'], range(2)))
                assert sorted(iterations) == [3, 4], iterations
            # A blocked network call must not hold the global pool mutex.
            with concurrent.futures.ThreadPoolExecutor() as pool:
                future = pool.submit(preview, 'slow', sid)
                assert Fixture.slow_started.wait(5)
                start = time.monotonic()
                preview('inspect', fail_sid)
                assert time.monotonic() - start < .8, 'one preview session blocked another'
                future.result()
            # Rejected requests do not consume a session's execution budget.
            for _ in range(3):
                preview('inspect', 'bad', error=True)
            limited = preview('inspect', context={'input': {'id': 7}})
            for _ in range(255):
                assert preview('inspect', limited['session_id'])['output']['selected'] == 7
            preview('inspect', limited['session_id'], error=True)
            while len(sessions) < 16:
                preview('inspect', context={'input': {'id': 7}})
            preview('inspect', context={'input': {'id': 7}}, error=True)
            api('/api/preview/release', {'session_id': limited['session_id']})
            api('/api/preview/release', {'session_id': limited['session_id']}, error=True)
            preview('inspect', limited['session_id'], error=True)
            replacement = preview('inspect', context={'input': {'id': 8}})
            assert replacement['session_id'] != limited['session_id']
            api('/api/preview/release', {'session_id': 'invalid'}, error=True)
            api('/api/preview/release', {'session_id': '0' * 64}, error=True)
            # Releasing a busy session denies new work immediately; the in-flight
            # handler owns its reference until completion, then capacity returns.
            Fixture.slow_started.clear()
            with concurrent.futures.ThreadPoolExecutor() as pool:
                future = pool.submit(preview, 'slow', sid)
                assert Fixture.slow_started.wait(5)
                queued = pool.submit(preview, 'inspect', sid, True)
                api('/api/preview/release', {'session_id': sid})
                preview('inspect', sid, error=True)
                future.result()
                queued.result()
            preview('inspect', context={'input': {'id': 9}})
            with sqlite3.connect(pathlib.Path(data, 'spider.db')) as db:
                for table in ('runs', 'items', 'artifacts', 'sessions', 'step_instances'):
                    assert db.execute('SELECT count(*) FROM ' + table).fetchone()[0] == 0, table
            assert not list(pathlib.Path(data, 'downloads').iterdir())
            proc.terminate()
            assert proc.wait(10) == 0
            print('preview integration passed')
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            log.seek(0)
            text = log.read()
            if sys.exc_info()[0]:
                print(text[-5000:], file=sys.stderr)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text[-5000:]
    fixture.shutdown()


if __name__ == '__main__':
    main()
