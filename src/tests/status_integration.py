"""Real code registration -> schedule -> success/failure -> read-only production Web."""
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

def main():
    binary=str(pathlib.Path(sys.argv[1]).resolve())
    product=str(pathlib.Path(sys.argv[2]).resolve())
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    ui=str(pathlib.Path(__file__).resolve().parents[1]/'web/ui')
    def api(path,method='GET',body=None,expected=200,token=True,control=False):
        headers={'Authorization':'Bearer fixture-token'} if token else {}
        if body is not None:headers['Content-Type']='application/json'
        if control:headers['X-Spider-Request']='1'
        request=urllib.request.Request(base+path,method=method,headers=headers,data=None if body is None else json.dumps(body).encode())
        try:response=opener.open(request,timeout=5)
        except urllib.error.HTTPError as error:response=error
        with response:
            data=response.read();assert response.status==expected,(method,path,response.status,data)
            assert b'fixture-secret' not in data,(path,data)
            return json.loads(data)
    with tempfile.TemporaryDirectory(prefix='spider-status-') as directory,tempfile.TemporaryFile(mode='w+') as log:
        processes=[]
        def start(executable,extra=()):
            process=subprocess.Popen([executable,'--port',str(port),'--data-dir',directory,'--ui-dir',ui,*extra],stdout=subprocess.DEVNULL,stderr=log)
            processes.append(process)
            for _ in range(150):
                assert process.poll() is None,'service exited'
                try:api('/api/health');return process
                except OSError:time.sleep(.03)
            raise AssertionError('service not ready')
        def stop(process):process.terminate();assert process.wait(timeout=10)==0
        try:
            process=start(binary)
            api('/api/spiders',expected=401,token=False)
            rows=api('/api/spiders');assert {row['code_id'] for row in rows}=={'good','bad','idle'},rows
            data=api('/api/library?type=music&page=1&page_size=20')
            assert data['total']==0 and data['items']==[],data
            api('/api/library?type=music&page=1&page_size=10',expected=400)
            ids={row['code_id']:row['id'] for row in rows}
            versions={row['code_id']:row['version'] for row in rows}
            assert all('rule' not in row and 'config' not in row for row in rows)
            for _ in range(200):
                rows=api('/api/spiders');by_key={row['code_id']:row for row in rows}
                if by_key['good']['successes'] and by_key['bad']['failures']:break
                time.sleep(.02)
            else:raise AssertionError(rows)
            assert by_key['idle']['latest_run_id'] is None
            api(f"/api/spiders/{ids['idle']}/run",'POST',expected=400)
            started=api(f"/api/spiders/{ids['idle']}/run",'POST',expected=201,control=True)
            for _ in range(200):
                history=api(f"/api/runs?spider_id={ids['idle']}")
                manual=next((row for row in history if row['id']==started['id']),None)
                if manual and manual.get('finished_at') is not None:break
                time.sleep(.02)
            else:raise AssertionError(started)
            assert manual['status']=='succeeded',manual
            for key in ['good','bad']:
                history=api(f"/api/runs?spider_id={ids[key]}");assert history
                terminal=next(row for row in history if row['status'] in ('succeeded','failed'))
                run=api(f"/api/runs/{terminal['id']}")
                assert 'snapshot' not in run and 'config' not in run
                assert run['finished_at'] is not None
                if key=='bad':assert run['error']=='fixture download failed'
                api(f"/api/runs/{terminal['id']}/events")
            for method,path in [('POST','/api/spiders'),('PUT',f"/api/spiders/{ids['good']}"),('DELETE',f"/api/spiders/{ids['good']}"),('POST','/api/preview'),('POST','/api/runs/1/stop')]:
                api(path,method,{},405)
            for method in ['PUT','DELETE']:
                api(f"/api/spiders/{ids['good']}/run",method,{},405)
            api('/api/runs/1',expected=404)
            api('/api/spiders/1',expected=404)
            stop(process)
            process=start(binary)
            rows=api('/api/spiders')
            assert {row['code_id']:row['id'] for row in rows}==ids
            assert {row['code_id']:row['version'] for row in rows}==versions
            stop(process)
            process=start(binary,['--remove-failed'])
            rows=api('/api/spiders');assert {row['code_id'] for row in rows}=={'good','idle'}
            assert {row['code_id']:row['id'] for row in rows}=={key:ids[key] for key in ['good','idle']}
            api(f"/api/spiders/{ids['bad']}",expected=404)
            stop(process)
            # The real product registers its own code task; fixture history stays dormant.
            process=start(product)
            product_rows=api('/api/spiders')
            assert {row['code_id'] for row in product_rows}=={'archive-org-chinese-music','archive-org-download-0','archive-org-download-1','archive-org-download-2'},product_rows
            assert api('/api/runs')==[]
            stop(process)
            with sqlite3.connect(pathlib.Path(directory,'spider.db')) as db:
                assert db.execute('SELECT count(*) FROM runs').fetchone()[0]>0
                enabled={row[0] for row in db.execute('SELECT name FROM spiders WHERE enabled=1')}
                assert enabled<={'archive.org 华语音乐','archive.org 华语音乐下载 1/3','archive.org 华语音乐下载 2/3','archive.org 华语音乐下载 3/3'},enabled
                assert db.execute('SELECT count(*) FROM runs WHERE status IN (\'queued\',\'running\')').fetchone()[0]==0
            print('code status integration passed: schedules, outcomes, stable IDs, removals, read-only API')
        finally:
            for process in processes:
                if process.poll() is None:process.kill();process.wait()
            log.seek(0);text=log.read()
            if sys.exc_info()[0]:print(text[-8000:],file=sys.stderr)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text,text

if __name__=='__main__':main()
