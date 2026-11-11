"""Production Web must reject every mutation, including requests without a UI."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    def api(path,method='GET',body=None,expected=200,control=True):
        data=None if body is None else json.dumps(body).encode()
        headers={'Content-Type':'application/json'}
        if control: headers['X-Spider-Request']='1'
        request=urllib.request.Request(base+path,method=method,data=data,headers=headers)
        try: response=opener.open(request,timeout=5)
        except urllib.error.HTTPError as error: response=error
        with response:
            value=response.read()
            assert response.status==expected,(method,path,response.status,value)
            return value
    with tempfile.TemporaryDirectory(prefix='spider-readonly-') as directory, tempfile.TemporaryFile(mode='w+') as log:
        process=subprocess.Popen([binary,'--port',str(port),'--data-dir',directory,'--ui-dir',
            str(pathlib.Path(__file__).resolve().parents[1]/'web/ui')],stdout=subprocess.DEVNULL,stderr=log)
        try:
            for _ in range(100):
                assert process.poll() is None,'service exited'
                try: api('/api/health'); break
                except OSError: time.sleep(.03)
            rule={'schema_version':2,'name':'forbidden','entry':'save','steps':[{'id':'save','type':'emit','fields':{}}]}
            api('/api/spiders','POST',rule,405)
            for path in ['/api/spiders/1','/api/runs/1/stop','/api/preview','/api/preview/step','/api/preview/release']:
                for method in ['POST','PUT','DELETE']:
                    api(path,method,{},405)
            for method in ['PUT','DELETE']:
                api('/api/spiders/1/run',method,{},405)
            api('/api/spiders/1/run','POST',{},expected=400,control=False)
            api('/api/spiders/99999/run','POST',{},expected=404)
            rows=json.loads(api('/api/spiders'))
            assert [row['code_id'] for row in rows]==['archive-org-chinese-music'], rows
            assert json.loads(api('/api/runs'))==[]
            api('/api/spiders/99999',expected=404)
            api('/api/implementations',expected=404)
            process.terminate(); assert process.wait(timeout=10)==0
            print('production Web: status is read-only, manual run is guarded')
        finally:
            if process.poll() is None: process.kill();process.wait()
            log.seek(0); text=log.read()
            if sys.exc_info()[0]: print(text[-6000:],file=sys.stderr)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text,text

if __name__=='__main__': main()
