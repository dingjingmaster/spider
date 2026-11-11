"""Exercise real C service against an isolated local HTTP fixture."""
import hashlib
import http.server
import json
import os
import pathlib
import signal
import sqlite3
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))


class Fixture(http.server.BaseHTTPRequestHandler):
    retries = 0
    captured = []
    slow_started = threading.Event()
    file_started = threading.Event()
    post_bodies = []

    def do_POST(self):
        Fixture.post_bodies.append((self.path, self.rfile.read(int(self.headers.get("Content-Length", "0")))))
        next_link = f'<a class="next" href="http://localhost:{self.server.server_port}/post-external">Next</a>' if self.path == "/post-origin" else ""
        body = ('<div class="row"><h2>POST</h2></div>' + next_link).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/protected") and self.headers.get("X-Api-Key") != "fixture-key":
            self.send_response(403)
            self.end_headers()
            return
        if self.path.startswith("/capture"):
            Fixture.captured.append(dict(self.headers))
        if self.path.startswith("/slow"):
            Fixture.slow_started.set()
        if self.path.startswith("/slow-file"):
            Fixture.file_started.set()
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", f"http://localhost:{self.server.server_port}/capture-redirect")
            self.end_headers()
            return
        if self.path == "/retry":
            Fixture.retries += 1
            if Fixture.retries == 1:
                self.send_response(503)
                self.end_headers()
                return
        if self.path.startswith("/slow"):
            time.sleep(3)
        if self.path == "/api":
            body = b'{"rows":[{"name":"API","price":42}]}'
        elif self.path in ("/file", "/slow-file", "/protected-file"):
            body = b"downloaded\x00file\xff"
        elif self.path == "/protected-list":
            body = b'<div class="row"><h2>Protected</h2><b>1</b><a href="/protected-file">File</a></div><a class="next" href="/protected-page2">Next</a>'
        elif self.path == "/protected-page2":
            body = b'<div class="row"><h2>Protected second</h2><b>2</b><a href="/protected-file">File</a></div>'
        elif self.path == "/cross":
            body = f'<div class="row"><h2>Cross</h2><b>1</b><a href="http://localhost:{self.server.server_port}/capture-file">File</a></div><a class="next" href="http://localhost:{self.server.server_port}/capture-page">Next</a>'.encode()
        elif self.path == "/file-page":
            body = b'<div class="row"><h2>File</h2><b>1</b><a href="/slow-file">File</a></div>'
        elif self.path == "/big":
            body = b"x" * 10000
        elif self.path == "/page2":
            body = b'<div class="row"><h2>Second</h2><b>20</b><a href="/file">File</a></div>'
        else:
            body = b'<div class="row"><h2>A &amp; B</h2><b>12.5</b><a href="/file">File</a></div><a class="next" href="/page2">Next</a>'
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def workflow(recipe):
    """Build ordinary v2 steps for this fixture's compact Python test recipes."""
    fields = [{key: value for key, value in field.items() if key != "download"}
              for field in recipe["fields"]]
    request = {key: recipe[key] for key in ("method", "headers", "body") if key in recipe}
    request.update(url="{{input.url}}", headers_origin="{{run.start_url}}")
    extract = {key: recipe[key] for key in ("format", "row_selector", "next_selector") if key in recipe}
    extract["fields"] = fields
    downloads = [field for field in recipe["fields"] if field.get("download")]
    steps = [
        {"id": "start", "type": "join", "fields": {"url": "{{run.start_url}}"},
         "next_input": "{{steps.start}}", "next": "page"},
        {"id": "page", "type": "request", "request": request, "extract": extract, "next": "rows"},
        {"id": "rows", "type": "foreach", "items": "{{steps.page.items}}",
         "body": "has_file_0" if downloads else "save", "next": "has_next"},
        {"id": "save", "type": "emit",
         "fields": {field["name"]: "{{input." + field["name"] + "}}" for field in fields}},
        {"id": "has_next", "type": "condition",
         "test": {"left": "{{steps.page.next_url}}", "op": "exists"}, "then": "under_limit"},
        {"id": "under_limit", "type": "condition",
         "test": {"left": "{{steps.page.iteration}}", "op": "lt", "right": recipe.get("max_pages", 1)},
         "then": "advance"},
        {"id": "advance", "type": "join", "fields": {"url": "{{steps.page.next_url}}"},
         "next_input": "{{steps.advance}}", "next": "page"},
    ]
    for index, field in enumerate(downloads):
        next_step = f"has_file_{index + 1}" if index + 1 < len(downloads) else "save"
        url = "{{input." + field["name"] + "}}"
        steps.extend([
            {"id": f"has_file_{index}", "type": "condition", "test": {"left": url, "op": "exists"},
             "then": f"file_{index}", "else": next_step},
            {"id": f"file_{index}", "type": "download", "url": url,
             "headers": recipe.get("headers", {}), "headers_origin": "{{run.start_url}}", "next": next_step},
        ])
    defaults = {key: recipe[key] for key in ("delay_ms", "retries", "timeout_seconds", "max_bytes") if key in recipe}
    defaults.setdefault("delay_ms", 0)
    return {"schema_version": 2, "name": recipe["name"], "entry": "start",
            "vars": {"start_url": recipe["url"]}, "defaults": defaults, "steps": steps,
            "fields": [{"name": field["name"]} for field in fields],
            "interval_seconds": recipe.get("interval_seconds", 0), "enabled": recipe.get("enabled", True)}


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    fixture = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    target = f"http://127.0.0.1:{fixture.server_port}"
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    ui = pathlib.Path(__file__).resolve().parents[1] / "web/ui"
    processes = []

    def api(path, method="GET", data=None, expected=200, marker=True, token=""):
        headers = {"Content-Type": "application/json"}
        if marker:
            headers["X-Spider-Request"] = "1"
        if token:
            headers["Authorization"] = "Bearer " + token
        req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(), headers=headers, method=method)
        try:
            response = urllib.request.urlopen(req, timeout=12)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            body = response.read()
            assert response.status == expected, (path, response.status, body)
            return json.loads(body) if "application/json" in response.headers.get("Content-Type", "") else body

    def recipe_api(path, method, recipe, expected=200, **kwargs):
        definition = workflow(recipe)
        payload = ({"rule": definition, "step_id": "page", "context": {"input": {"url": recipe["url"]}}}
                   if path == "/api/preview" else definition)
        return api(path, method, payload, expected, **kwargs)

    def start(data_dir, token=""):
        command = [binary, "--port", str(port), "--data-dir", data_dir, "--ui-dir", str(ui)]
        if token:
            command += ["--token", token]
        env = {**os.environ, "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost"}
        p = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log, env=env)
        processes.append(p)
        for _ in range(100):
            assert p.poll() is None, "service exited during startup"
            try:
                api("/api/health", token=token)
                return p
            except (OSError, AssertionError):
                time.sleep(0.05)
        raise AssertionError("service did not start")

    def wait_run(run):
        for _ in range(150):
            status = api(f"/api/runs/{run}")
            if status["status"] not in ("queued", "running"):
                return status
            time.sleep(0.05)
        raise AssertionError("run did not finish")

    with tempfile.TemporaryDirectory(prefix="spider-integration-") as data_dir, tempfile.TemporaryFile(mode="w+") as log:
        try:
            p = start(data_dir)
            assert b"Spider" in api("/")
            rule = {"name": "Fixture", "url": target + "/", "format": "html", "row_selector": ".row", "next_selector": ".next", "max_pages": 2, "delay_ms": 0, "fields": [{"name": "title", "selector": "h2", "required": True}, {"name": "price", "selector": "b", "type": "number"}, {"name": "file", "selector": "a", "attribute": "href", "download": True}]}
            # The external API accepts only workflow definitions, including preview.
            api("/api/spiders", "POST", rule, 400)
            api("/api/preview", "POST", rule, 400)
            api("/api/spiders", "POST", {**rule, "schema_version": 1}, 400)
            recipe_api("/api/preview", "POST", {**rule, "url": "file:///etc/passwd"}, 422)
            recipe_api("/api/preview", "POST", rule, 403, marker=False)
            recipe_api("/api/preview", "POST", {**rule, "row_selector": "["}, 400)
            preview = recipe_api("/api/preview", "POST", rule)
            assert preview["items"][0]["title"] == "A & B"
            assert preview["items"][0]["price"] == 12.5
            assert preview["next_url"] == target + "/page2"
            assert not list(pathlib.Path(data_dir, "downloads").glob("*")), "preview downloaded files"
            sid = recipe_api("/api/spiders", "POST", rule, 201)["id"]
            api(f"/api/spiders/{sid}", "PUT", rule, 400)
            assert api("/api/spiders")[0]["id"] == sid
            run = api(f"/api/spiders/{sid}/run", "POST", {}, 201)["id"]
            done = wait_run(run)
            assert done["status"] == "succeeded", done
            assert done["pages"] == 2 and done["item_count"] == 2, done
            items = api(f"/api/runs/{run}/items")
            assert [i["data"]["title"] for i in items] == ["A & B", "Second"]
            files = api(f"/api/runs/{run}/artifacts")
            assert len(files) == 1
            payload = api(f'/api/artifacts/{files[0]["id"]}/file')
            assert payload == b"downloaded\x00file\xff"
            assert files[0]["sha256"] == hashlib.sha256(payload).hexdigest()
            assert b'"title"' in api(f"/api/runs/{run}/export?format=csv")
            assert len(api(f"/api/runs/{run}/export?format=json")) == 2
            recipe_api("/api/preview", "POST", {**rule, "url": target + "/big", "max_bytes": 100}, 422)
            jr = {"name": "JSON", "url": target + "/api", "format": "json", "row_selector": "/rows", "fields": [{"name": "title", "selector": "/name", "required": True}]}
            assert recipe_api("/api/preview", "POST", jr)["items"] == [{"title": "API"}]
            retry = recipe_api("/api/spiders", "POST", {**rule, "name": "retry", "url": target + "/retry", "max_pages": 1}, 201)["id"]
            rr = api(f"/api/spiders/{retry}/run", "POST", {}, 201)["id"]
            assert wait_run(rr)["status"] == "succeeded"
            assert Fixture.retries == 2
            # Fault injection: failure to persist the HTTP attempt must fail the run.
            with sqlite3.connect(pathlib.Path(data_dir, "spider.db")) as db:
                db.execute("CREATE TRIGGER reject_request BEFORE INSERT ON requests BEGIN SELECT RAISE(ABORT,'request persistence rejected'); END")
            fault = api(f"/api/spiders/{retry}/run", "POST", {}, 201)["id"]
            assert wait_run(fault)["status"] == "failed", "queue failure reported success"
            with sqlite3.connect(pathlib.Path(data_dir, "spider.db")) as db:
                db.execute("DROP TRIGGER reject_request")
            cross = recipe_api("/api/spiders", "POST", {**rule, "name": "cross", "url": target + "/cross", "headers": {"Authorization": "Bearer fixture-secret", "Cookie": "session=secret", "X-Api-Key": "secret"}}, 201)["id"]
            cr = api(f"/api/spiders/{cross}/run", "POST", {}, 201)["id"]
            assert wait_run(cr)["status"] == "succeeded"
            recipe_api("/api/preview", "POST", {**rule, "url": target + "/redirect", "headers": {"X-Api-Key": "secret"}})
            assert len(Fixture.captured) >= 3
            assert all(not any(k.lower() in ("authorization", "cookie", "x-api-key") for k in h) for h in Fixture.captured), "credentials leaked to another origin"
            authenticated = recipe_api("/api/spiders", "POST", {**rule, "name": "Authenticated files", "url": target + "/protected-list", "headers": {"X-Api-Key": "fixture-key"}}, 201)["id"]
            ar = api(f"/api/spiders/{authenticated}/run", "POST", {}, 201)["id"]
            assert wait_run(ar)["status"] == "succeeded"
            assert len(api(f"/api/runs/{ar}/artifacts")) == 1
            slow = recipe_api("/api/spiders", "POST", {**rule, "name": "Slow", "url": target + "/slow", "max_pages": 1}, 201)["id"]
            sr = api(f"/api/spiders/{slow}/run", "POST", {}, 201)["id"]
            api(f"/api/spiders/{slow}/run", "POST", {}, 409)
            recipe_api(f"/api/spiders/{slow}", "PUT", {**rule, "name": "Edited"})
            assert api(f"/api/runs/{sr}")["snapshot"]["name"] == "Slow"
            api(f"/api/runs/{sr}/stop", "POST", {})
            assert wait_run(sr)["status"] == "cancelled"
            # Wait for a real in-flight fetch before testing graceful recovery.
            recipe_api(f"/api/spiders/{slow}", "PUT", {**rule, "name": "Slow recovery", "url": target + "/slow", "max_pages": 1})
            Fixture.slow_started.clear()
            for _ in range(200):
                request = urllib.request.Request(base + f"/api/spiders/{slow}/run", data=b"{}", method="POST", headers={"Content-Type":"application/json", "X-Spider-Request":"1"})
                try:
                    with urllib.request.urlopen(request, timeout=5) as response:
                        assert response.status == 201
                        recover = json.loads(response.read())["id"]
                    break
                except urllib.error.HTTPError as error:
                    assert error.code == 409
                    error.close()
                    time.sleep(.025)
            else:
                raise AssertionError("cancelled crawler did not release its execution slot")
            assert Fixture.slow_started.wait(5)
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=10) == 0
            p = start(data_dir)
            recovered = wait_run(recover)
            assert recovered["status"] == "succeeded" and recovered["pages"] == 1 and recovered["item_count"] == 1, recovered
            # Artifact fetch interrupted by shutdown must also resume.
            recipe_api(f"/api/spiders/{slow}", "PUT", {**rule, "name": "File recovery", "url": target + "/file-page", "max_pages": 1})
            Fixture.file_started.clear()
            recover = api(f"/api/spiders/{slow}/run", "POST", {}, 201)["id"]
            assert Fixture.file_started.wait(5)
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=10) == 0
            p = start(data_dir)
            recovered = wait_run(recover)
            assert recovered["status"] == "succeeded" and recovered["item_count"] == 1, recovered
            assert len(api(f"/api/runs/{recover}/artifacts")) == 1
            # Abrupt termination keeps the in-flight request durable too.
            recipe_api(f"/api/spiders/{slow}", "PUT", {**rule, "name": "Crash recovery", "url": target + "/slow", "max_pages": 1})
            Fixture.slow_started.clear()
            recover = api(f"/api/spiders/{slow}/run", "POST", {}, 201)["id"]
            assert Fixture.slow_started.wait(5)
            p.kill()
            p.wait(timeout=5)
            p = start(data_dir)
            recovered = wait_run(recover)
            assert recovered["status"] == "succeeded" and recovered["item_count"] == 1, recovered
            scheduled = recipe_api("/api/spiders", "POST", {**jr, "interval_seconds": 1}, 201)["id"]
            for _ in range(60):
                runs = api(f"/api/runs?spider_id={scheduled}")
                if runs and runs[0]["status"] == "succeeded":
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("interval scheduler did not run")
            recipe_api(f"/api/spiders/{scheduled}", "PUT", {**jr, "interval_seconds": 0})
            api(f"/api/spiders/{sid}", "DELETE")
            assert all(s["id"] != sid for s in api("/api/spiders"))
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=10) == 0
            # Simulate an installed v2 database containing a real legacy current rule.
            # Startup must migrate it before the workflow-only run API is used.
            legacy = {**rule, "name": "Migrated installed rule", "url": target + "/protected-list", "headers": {"X-Api-Key": "fixture-key"}}
            legacy_post = {**rule, "name": "Migrated POST origin", "url": target + "/post-origin", "method": "POST", "body": "password=secret&literal={{untouched}}", "fields": [{"name": "title", "selector": "h2"}]}
            stamp = int(time.time() * 1000)
            with sqlite3.connect(pathlib.Path(data_dir, "spider.db")) as db:
                migrated_sid = db.execute(
                    "INSERT INTO spiders(name,rule_json,updated_at) VALUES(?,?,?)",
                    (legacy["name"], json.dumps(legacy), stamp)).lastrowid
                db.execute("INSERT INTO rule_versions(spider_id,version,rule_json,created_at) VALUES(?,1,?,?)",
                           (migrated_sid, json.dumps(legacy), stamp))
                post_sid = db.execute("INSERT INTO spiders(name,rule_json,updated_at) VALUES(?,?,?)", (legacy_post["name"], json.dumps(legacy_post), stamp)).lastrowid
                db.execute("INSERT INTO rule_versions(spider_id,version,rule_json,created_at) VALUES(?,1,?,?)", (post_sid, json.dumps(legacy_post), stamp))
                db.execute("PRAGMA user_version=2")
            p = start(data_dir)
            with sqlite3.connect(pathlib.Path(data_dir, "spider.db")) as db:
                assert db.execute("PRAGMA user_version").fetchone()[0] == 3
                current = json.loads(db.execute("SELECT rule_json FROM spiders WHERE id=?", (migrated_sid,)).fetchone()[0])
                assert current["schema_version"] == 2, current
            migrated_run = api(f"/api/spiders/{migrated_sid}/run", "POST", {}, 201)["id"]
            migrated_done = wait_run(migrated_run)
            assert migrated_done["status"] == "succeeded", migrated_done
            assert migrated_done["pages"] == 2 and migrated_done["item_count"] == 2, migrated_done
            migrated_items = [item["data"] for item in api(f"/api/runs/{migrated_run}/items")]
            assert migrated_items == [{"title": "Protected", "price": 1, "file": target + "/protected-file"},
                                      {"title": "Protected second", "price": 2, "file": target + "/protected-file"}], migrated_items
            migrated_files = api(f"/api/runs/{migrated_run}/artifacts")
            assert len(migrated_files) == 1, migrated_files
            assert api(f'/api/artifacts/{migrated_files[0]["id"]}/file') == b"downloaded\x00file\xff"
            post_run = api(f"/api/spiders/{post_sid}/run", "POST", {}, 201)["id"]
            post_done = wait_run(post_run)
            assert post_done["status"] == "failed", post_done
            assert "Cross-origin POST" in post_done["error"], post_done
            assert Fixture.post_bodies == [("/post-origin", b"password=secret&literal={{untouched}}")], Fixture.post_bodies
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=10) == 0
            p = start(data_dir, "test-secret")
            api("/api/spiders", expected=401)
            api("/api/spiders", token="test-secret")
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=10) == 0
            print("integration tests passed")
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                    p.wait()
            log.seek(0)
            output = log.read()
            if "AddressSanitizer" in output or "runtime error:" in output:
                print(output, file=sys.stderr)
                raise AssertionError("sanitizer reported an error")
            if sys.exc_info()[0]:
                print(output[-5000:], file=sys.stderr)
    fixture.shutdown()


if __name__ == "__main__":
    main()
