"""Workflow execution uses a real C process, HTTP server and SQLite file."""
import collections
import http.server
import json
import os
import pathlib
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
counts = collections.Counter()
slow = threading.Event()

class Site(http.server.BaseHTTPRequestHandler):
    def send(self, value, code=200, cookie=False):
        data = json.dumps(value).encode()
        self.send_response(code)
        self.send_header('Content-Length', str(len(data)))
        if cookie:
            self.send_header('Set-Cookie', 'sid=workflow; Path=/; HttpOnly')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        counts[url.path] += 1
        if url.path == '/login':
            return self.send({'token': 'step-token'}, cookie=True)
        if url.path == '/retry' and counts[url.path] == 1:
            return self.send({}, 503)
        if url.path.startswith('/slow'):
            assert self.headers.get('Cookie') == 'sid=workflow', dict(self.headers)
            slow.set()
            time.sleep(2)
            return self.send({'value': 7})
        if url.path == '/list':
            assert self.headers.get('Cookie') == 'sid=workflow', dict(self.headers)
            assert self.headers.get('X-Token') == 'step-token'
            assert query['q'] == ['a&b c']
            return self.send({'rows': [{'id':'A','title':'Name A','file':f'http://127.0.0.1:{self.server.server_port}/file'}, {'id':'B','title':'Name B','file':None}]})
        if url.path == '/html-list':
            data=b'<a class="item" href="/json-detail?id=C">Charlie</a>'
            self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
        if url.path == '/json-detail':
            return self.send({'description':'Detail '+query['id'][0]})
        if url.path == '/pages':
            page=int(query.get('page',['1'])[0])
            return self.send({'rows':[{'id':page}],'next':f'http://127.0.0.1:{self.server.server_port}/pages?page={page+1}' if page<4 else None})
        if url.path == '/detail':
            return self.send({'description':'Detail '+query['id'][0]})
        if url.path == '/sometimes':
            return self.send({'description':'Detail '+query['id'][0], **({'absent':'present'} if query['id'][0]=='A' else {})})
        if url.path == '/stock':
            time.sleep(.15)
            return self.send({'stock': 11 if query['id'][0]=='A' else 22})
        if url.path == '/missing':
            return self.send({}, 404)
        if url.path == '/file':
            assert self.headers.get('Cookie') == 'sid=workflow'
            return self.send({'file':'payload'})
        return self.send({'ok':True})

    def do_POST(self):
        counts[self.path] += 1
        value = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
        assert value['q'] == 'a&b c'
        if self.path == '/slow-post':
            slow.set();time.sleep(2);return self.send({'ok':True})
        time.sleep(.15)
        self.send({'price': 10.5 if value['id']=='A' else 20.5})

    def log_message(self, *_):
        pass

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0))
        return sock.getsockname()[1]

def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Site)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    target = f'http://127.0.0.1:{server.server_port}'
    port = free_port()
    base = f'http://127.0.0.1:{port}'
    ui = pathlib.Path(__file__).resolve().parents[1]/'web/ui'

    def api(path, method='GET', data=None, expected=200):
        request = urllib.request.Request(base+path, data=None if data is None else json.dumps(data).encode(),method=method,headers={'Content-Type':'application/json','X-Spider-Request':'1'})
        try: response=opener.open(request,timeout=10)
        except urllib.error.HTTPError as error: response=error
        with response:
            body=response.read()
            assert response.status==expected,(path,response.status,body)
            return json.loads(body) if 'application/json' in response.headers.get('Content-Type','') else body

    def wait(run):
        for _ in range(300):
            row=api(f'/api/runs/{run}')
            if row['status'] not in ('queued','running'):
                return row
            time.sleep(.05)
        raise AssertionError('Workflow stalled: '+str(api(f'/api/runs/{run}/steps')))

    def fields(*names):
        return {'format':'json','fields':[{'name':name,'selector':'/'+name,'required':True,
            'type':'number' if name=='price' else 'integer' if name in ('stock','value') else 'string'} for name in names]}

    rule={'schema_version':2,'name':'complete workflow','entry':'login','vars':{'base':target,'q':'a&b c'},'defaults':{'delay_ms':0,'retries':1},'steps':[
        {'id':'login','type':'request','request':{'url':'{{run.base}}/login'},'extract':fields('token'),'next':'list'},
        {'id':'list','type':'request','request':{'url':'{{run.base}}/list','headers':{'X-Token':'{{steps.login.items.0.token}}'},'query':{'q':'{{run.q}}'}},'extract':{'format':'json','row_selector':'/rows','fields':[{'name':'id','selector':'/id'},{'name':'title','selector':'/title'},{'name':'file','selector':'/file'}]},'next':'each'},
        {'id':'each','type':'foreach','items':'{{steps.list.items}}','body':'detail'},
        {'id':'detail','type':'request','request':{'url':'{{run.base}}/detail','query':{'id':'{{input.id}}'}},'extract':fields('description'),'next':'extra'},
        {'id':'extra','type':'parallel','branches':{'price':'price','stock':'stock'},'next':'has_file'},
        {'id':'price','type':'request','request':{'url':'{{run.base}}/price','method':'POST','json':{'id':'{{input.id}}','q':'{{run.q}}'}},'extract':fields('price')},
        {'id':'stock','type':'request','request':{'url':'{{run.base}}/stock','query':{'id':'{{input.id}}'}},'extract':fields('stock')},
        {'id':'has_file','type':'condition','test':{'left':'{{input.file}}','op':'exists'},'then':'file','else':'save'},
        {'id':'file','type':'download','url':'{{input.file}}','next':'save'},
        {'id':'save','type':'emit','fields':{'id':'{{input.id}}','title':'{{input.title}}','description':'{{steps.detail.items.0.description}}','price':'{{steps.extra.price.items.0.price}}','stock':'{{steps.extra.stock.items.0.stock}}'}}]}
    processes=[]
    with tempfile.TemporaryDirectory(prefix='spider-flow-') as directory,tempfile.TemporaryFile(mode='w+') as log:
        def start():
            env={**os.environ,'NO_PROXY':'127.0.0.1,localhost','no_proxy':'127.0.0.1,localhost'}
            p=subprocess.Popen([binary,'--port',str(port),'--workers','4','--data-dir',directory,'--ui-dir',str(ui)],stdout=subprocess.DEVNULL,stderr=log,env=env)
            processes.append(p)
            for _ in range(100):
                assert p.poll() is None,'service exited'
                try: api('/api/health');return p
                except OSError:time.sleep(.05)
            raise AssertionError('service unavailable')

        def run_rule(value):
            sid=api('/api/spiders','POST',value,201)['id']
            return api(f'/api/spiders/{sid}/run','POST',{},201)['id']

        try:
            p=start()
            bad=json.loads(json.dumps(rule));bad['entry']='not_defined'
            api('/api/spiders','POST',bad,400)
            preview=api('/api/preview','POST',{'rule':rule,'step_id':'login','context':{}})
            assert preview['items']==[{'token':'step-token'}]
            run=run_rule(rule);result=wait(run)
            assert result['status']=='succeeded',result
            items=sorted([r['data'] for r in api(f'/api/runs/{run}/items')],key=lambda x:x['id'])
            assert items==[{'id':'A','title':'Name A','description':'Detail A','price':10.5,'stock':11},{'id':'B','title':'Name B','description':'Detail B','price':20.5,'stock':22}],items
            assert len(api(f'/api/runs/{run}/artifacts'))==1
            steps=api(f'/api/runs/{run}/steps')
            assert len([s for s in steps if s['step_id']=='detail'])==2
            assert all(s['status'] in ('done','skipped') for s in steps),steps
            assert all('context' not in s and 'output' not in s for s in steps)
            detail=api(f'/api/runs/{run}/steps/{steps[0]["id"]}')
            assert isinstance(detail['context'],dict) and 'output' in detail
            api(f'/api/runs/{run+1000}/steps/{steps[0]["id"]}',expected=404)
            assert len(api(f'/api/runs/{run}/export?format=json'))==2
            html={'schema_version':2,'name':'relative HTML detail','entry':'list','steps':[
                {'id':'list','type':'request','request':{'url':target+'/html-list'},'extract':{'format':'html','row_selector':'a.item','fields':[{'name':'title','selector':''},{'name':'url','selector':'','attribute':'href'}]},'next':'each'},
                {'id':'each','type':'foreach','items':'{{steps.list.items}}','body':'detail'},
                {'id':'detail','type':'request','request':{'url':'{{input.url}}'},'extract':fields('description'),'next':'save'},
                {'id':'save','type':'emit','fields':{'title':'{{input.title}}','description':'{{steps.detail.items.0.description}}'}}]}
            hr=run_rule(html);assert wait(hr)['status']=='succeeded'
            assert api(f'/api/runs/{hr}/items')[0]['data']=={'title':'Charlie','description':'Detail C'}
            # Controlled paging revisits the same page step, while each row gets its own scope.
            parser={'format':'json','row_selector':'/rows','fields':[{'name':'id','selector':'/id','type':'integer'}],'next_selector':'/next'}
            paging={'schema_version':2,'name':'workflow paging','entry':'start','vars':{'start':target+'/pages?page=1'},'steps':[
                {'id':'start','type':'join','fields':{'url':'{{run.start}}'},'next_input':'{{steps.start}}','next':'page'},
                {'id':'page','type':'request','request':{'url':'{{input.url}}'},'extract':parser,'next':'rows'},
                {'id':'rows','type':'foreach','items':'{{steps.page.items}}','body':'save','next':'more'},
                {'id':'more','type':'condition','test':{'left':'{{steps.page.next_url}}','op':'exists'},'then':'advance'},
                {'id':'advance','type':'join','fields':{'url':'{{steps.page.next_url}}'},'next_input':'{{steps.advance}}','next':'page'},
                {'id':'save','type':'emit','fields':{'id':'{{input.id}}'}}]}
            pg=run_rule(paging);assert wait(pg)['status']=='succeeded'
            assert [i['data']['id'] for i in api(f'/api/runs/{pg}/items')]==[1,2,3,4]
            # A failed record branch is skipped without losing the other record.
            partial=json.loads(json.dumps(rule));partial['name']='partial'
            detail=next(s for s in partial['steps'] if s['id']=='detail')
            detail['on_error']='skip';detail['extract']['fields'].append({'name':'absent','selector':'/absent','required':True})
            detail['request']['url']='{{run.base}}/sometimes'
            pr=run_rule(partial);assert wait(pr)['status']=='partial'
            assert [i['data']['id'] for i in api(f'/api/runs/{pr}/items')]==['A']
            retry={'schema_version':2,'name':'retry','entry':'fetch','defaults':{'delay_ms':0,'retries':1},'steps':[{'id':'fetch','type':'request','request':{'url':target+'/retry'},'next':'save'},{'id':'save','type':'emit','fields':{'ok':True}}]}
            rr=run_rule(retry);assert wait(rr)['status']=='succeeded';assert counts['/retry']==2
            ordered=json.loads(json.dumps(retry));ordered['name']='ordered export';ordered['steps'][0]['request']['url']=target+'/ok'
            ordered['fields']=[{'name':'b'},{'name':'a'}];ordered['steps'][1]['fields']={'a':1,'b':2,'excluded':3}
            er=run_rule(ordered);assert wait(er)['status']=='succeeded'
            assert api(f'/api/runs/{er}/export?format=csv').decode().splitlines()==['"b","a"','"2","1"']
            # Recovery resumes slow step, preserving the completed login and session.
            recovery={'schema_version':2,'name':'recovery','entry':'login','vars':{'base':target},'defaults':{'delay_ms':0},'steps':[rule['steps'][0]|{'next':'slow'},{'id':'slow','type':'request','request':{'url':target+'/slow'},'extract':fields('value'),'next':'save'},{'id':'save','type':'emit','fields':{'value':'{{steps.slow.items.0.value}}','token':'{{steps.login.items.0.token}}'}}]}
            slow.clear();before=counts['/login'];sr=run_rule(recovery);assert slow.wait(5)
            p.send_signal(signal.SIGTERM);assert p.wait(timeout=10)==0;p=start()
            result=wait(sr);assert result['status']=='succeeded',result
            assert counts['/login']==before+1,'completed login replayed'
            assert api(f'/api/runs/{sr}/items')[0]['data']['value']==7
            # The target may have accepted a POST before shutdown; do not replay by default.
            posting={'schema_version':2,'name':'uncertain POST','entry':'post','defaults':{'delay_ms':0,'retries':0},'steps':[
                {'id':'post','type':'request','request':{'url':target+'/slow-post','method':'POST','json':{'q':'a&b c'}},'next':'save'},
                {'id':'save','type':'emit','fields':{'ok':True}}]}
            slow.clear();before=counts['/slow-post'];post_run=run_rule(posting);assert slow.wait(5)
            p.send_signal(signal.SIGTERM);assert p.wait(timeout=10)==0;p=start()
            assert wait(post_run)['status']=='failed','ambiguous POST replayed after restart'
            assert counts['/slow-post']==before+1
            with sqlite3.connect(pathlib.Path(directory,'spider.db')) as db:
                assert db.execute('SELECT count(*) FROM sessions WHERE run_id=?',(post_run,)).fetchone()[0]==0,'terminal session retained'
            posting['steps'][0]['retry_post']=True
            slow.clear();before=counts['/slow-post'];post_run=run_rule(posting);assert slow.wait(5)
            p.send_signal(signal.SIGTERM);assert p.wait(timeout=10)==0;p=start()
            assert wait(post_run)['status']=='succeeded'
            assert counts['/slow-post']==before+2
            loop={'schema_version':2,'name':'bounded loop','entry':'loop','max_steps':5,'steps':[{'id':'loop','type':'condition','test':{'left':1,'op':'eq','right':1},'then':'loop'}]}
            lr=run_rule(loop);assert wait(lr)['status']=='failed'
            slow.clear();sr=run_rule(recovery);assert slow.wait(5);api(f'/api/runs/{sr}/stop','POST',{});assert wait(sr)['status']=='cancelled'
            p.send_signal(signal.SIGTERM);assert p.wait(timeout=10)==0
            print('workflow integration tests passed')
        finally:
            for p in processes:
                if p.poll() is None:p.kill();p.wait()
            log.seek(0);output=log.read()
            if 'AddressSanitizer' in output or 'runtime error:' in output:raise AssertionError(output)
            if sys.exc_info()[0]:print(output[-6000:],file=sys.stderr)
    server.shutdown()

if __name__=='__main__':main()
