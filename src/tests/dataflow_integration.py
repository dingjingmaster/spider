"""Typed JSON extraction, branch input contracts, transformations and request payloads."""
import http.server
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
received = []


class Site(http.server.BaseHTTPRequestHandler):
    def send(self, value):
        data = json.dumps(value).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.send({'id.key': 123, 'order': {'lines': [{'sku': 'A', 'qty': 2}]},
                   'enabled': False, 'token': 'private-test-token', 'label': '  First item  '})

    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        received.append(value)
        self.send({'received': value})

    def log_message(self, *_):
        pass


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    fixture = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Site)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    target = f'http://127.0.0.1:{fixture.server_port}'
    base = f'http://127.0.0.1:{port()}'

    def api(path, method='GET', data=None, expected=200):
        req = urllib.request.Request(base + path, method=method,
            data=None if data is None else json.dumps(data).encode(),
            headers={'Content-Type': 'application/json', 'X-Spider-Request': '1'})
        try:
            response = opener.open(req, timeout=10)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            body = response.read()
            assert response.status == expected, (path, response.status, body)
            return json.loads(body)

    def wait(run):
        for _ in range(100):
            state = api(f'/api/runs/{run}')
            if state['status'] not in ('queued', 'running'):
                return state
            time.sleep(.05)
        raise AssertionError('run did not finish')

    def run_rule(rule):
        sid = api('/api/spiders', 'POST', rule, 201)['id']
        return api(f'/api/spiders/{sid}/run', 'POST', {}, 201)['id']

    with tempfile.TemporaryDirectory(prefix='spider-dataflow-') as directory, tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), '--port', base.rsplit(':', 1)[1],
            '--data-dir', directory, '--ui-dir', str(pathlib.Path(__file__).resolve().parents[1] / 'web/ui'),
            '--workers', '4'], stdout=subprocess.DEVNULL, stderr=log,
            env={**os.environ, 'NO_PROXY': '127.0.0.1,localhost', 'no_proxy': '127.0.0.1,localhost'})
        try:
            for _ in range(100):
                assert process.poll() is None
                try:
                    api('/api/health')
                    break
                except OSError:
                    time.sleep(.05)
            else:
                raise AssertionError('service did not start')
            ref = lambda path: {'$ref': path}
            rule = {'schema_version': 2, 'name': 'Typed order', 'entry': 'source', 'vars': {'api': target},
                'defaults': {'delay_ms': 0}, 'steps': [
                    {'id': 'source', 'type': 'request', 'request': {'url': '{{run.api}}/source'},
                     'response_json': True, 'response_body': False,
                     'extract': {'format': 'json', 'fields': [
                         {'name': 'id.key', 'selector': '/id.key', 'type': 'integer'},
                         {'name': 'order', 'selector': '/order', 'type': 'object'},
                         {'name': 'enabled', 'selector': '/enabled', 'type': 'json'}]},
                     'next_input': {'id': ref('/steps/source/items/0/id.key'), 'order': ref('/steps/source/items/0/order'),
                                    'enabled': ref('/steps/source/items/0/enabled')}, 'next': 'clean'},
                    {'id': 'clean', 'type': 'transform', 'value': ref('/steps/source/response/json/label'),
                     'operations': [{'op': 'trim'}, {'op': 'replace', 'old': 'First', 'new': 'Second'}], 'next': 'submit'},
                    {'id': 'submit', 'type': 'request',
                     'input': {'id': ref('/input/id'), 'order': ref('/input/order'), 'enabled': ref('/input/enabled'),
                               'label': ref('/steps/clean')},
                     'input_schema': {'type': 'object', 'required': ['id', 'order', 'enabled', 'locale'],
                         'additionalProperties': False, 'properties': {
                             'id': {'type': 'integer'}, 'order': {'type': 'object'}, 'enabled': {'type': 'boolean'},
                             'label': {'type': 'string'}, 'locale': {'type': 'string', 'default': 'zh'}}},
                     'request': {'method': 'POST', 'url': '{{run.api}}/consume', 'json': ref('/input')},
                     'response_json': True, 'response_body': False, 'retain_steps': ['submit'], 'next': 'save'},
                    {'id': 'save', 'type': 'emit', 'fields': ref('/steps/submit/response/json/received')}]}
            run = run_rule(rule)
            done = wait(run)
            assert done['status'] == 'succeeded', done
            expected = {'id': 123, 'order': {'lines': [{'sku': 'A', 'qty': 2}]},
                        'enabled': False, 'label': 'Second item', 'locale': 'zh'}
            assert received == [expected], received
            assert api(f'/api/runs/{run}/items')[0]['data'] == expected
            steps = api(f'/api/runs/{run}/steps')
            source = next(step for step in steps if step['step_id'] == 'source')
            trace = api(f'/api/runs/{run}/steps/{source["id"]}')
            assert 'body' not in trace['output']['response']
            assert trace['output']['response']['json']['token'] != 'private-test-token'
            assert 'private-test-token' not in json.dumps(trace)
            submit = next(step for step in steps if step['step_id'] == 'submit')
            assert set(api(f'/api/runs/{run}/steps/{submit["id"]}')['context']['steps']) == {'submit'}
            derived = {'schema_version': 2, 'name': 'Derived secrets stay private', 'entry': 'source', 'steps': [
                {'id': 'source', 'type': 'request', 'request': {'url': target + '/source'},
                 'response_json': True, 'response_body': False, 'next': 'derived'},
                {'id': 'derived', 'type': 'transform', 'value': ref('/steps/source/response/json/token'),
                 'operations': [{'op': 'trim'}], 'retain_steps': ['derived'], 'next': 'save'},
                {'id': 'save', 'type': 'emit', 'fields': {'copied': ref('/steps/derived')}}]}
            dr = run_rule(derived)
            assert wait(dr)['status'] == 'succeeded'
            derived_step = next(s for s in api(f'/api/runs/{dr}/steps') if s['step_id'] == 'derived')
            assert 'private-test-token' not in json.dumps(api(f'/api/runs/{dr}/steps/{derived_step["id"]}'))
            assert api(f'/api/runs/{dr}/items')[0]['data']['copied'] == 'private-test-token', 'debug redaction altered execution'
            bad = {'schema_version': 2, 'name': 'Rejected input', 'entry': 'send', 'steps': [
                {'id': 'send', 'type': 'request', 'input': {'id': 'wrong'},
                 'input_schema': {'type': 'object', 'properties': {'id': {'type': 'integer'}}},
                 'request': {'method': 'POST', 'url': target + '/consume', 'json': ref('/input')}}]}
            failed = wait(run_rule(bad))
            assert failed['status'] == 'failed' and 'id' in failed['error'], failed
            assert len(received) == 1, 'invalid input reached the HTTP server'
            # Mapping an array before foreach must happen once, before child contexts are forked.
            fan = {'schema_version': 2, 'name': 'Independent JSON branches', 'entry': 'each', 'steps': [
                {'id': 'each', 'type': 'foreach', 'input': {'values': [{'id': 1}, {'id': 2}], 'marker': 17},
                 'items': ref('/input/values'), 'body': 'save', 'next': 'summary'},
                {'id': 'save', 'type': 'emit', 'input_schema': {'type': 'object', 'required': ['id']},
                 'fields': {'id': ref('/input/id')}},
                {'id': 'summary', 'type': 'emit', 'fields': {'marker': ref('/input/marker')}}]}
            fr = run_rule(fan)
            fan_done = wait(fr)
            assert fan_done['status'] == 'succeeded', fan_done
            fan_items = [i['data'] for i in api(f'/api/runs/{fr}/items')]
            assert sorted(i['id'] for i in fan_items if 'id' in i) == [1, 2]
            assert {'marker': 17} in fan_items, fan_items
            nil_rule = {'schema_version': 2, 'name': 'Null group values', 'entry': 'each', 'steps': [
                {'id': 'each', 'type': 'foreach', 'items': [1], 'body': 'nil', 'next': 'save'},
                {'id': 'nil', 'type': 'transform', 'value': None, 'operations': [{'op': 'default', 'value': None}]},
                {'id': 'save', 'type': 'emit', 'fields': {'joined': ref('/steps/each')}}]}
            nr = run_rule(nil_rule)
            null_done = wait(nr)
            assert null_done['status'] == 'succeeded', null_done
            assert api(f'/api/runs/{nr}/items')[0]['data'] == {'joined': [None]}
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=10) == 0
            print('JSON dataflow integration passed')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            log.seek(0)
            output = log.read()
            if 'AddressSanitizer' in output or 'runtime error:' in output:
                raise AssertionError(output)
            if sys.exc_info()[0]:
                print(output[-5000:], file=sys.stderr)
    fixture.shutdown()


if __name__ == '__main__':
    main()
