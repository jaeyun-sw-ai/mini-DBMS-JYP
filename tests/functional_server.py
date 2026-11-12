#!/usr/bin/env python3
"""Phase 3 기능 테스트: 실제 서버를 띄우고 HTTP로 호출한다 (정상/오류/동시 요청)."""
import json, os, shutil, signal, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
checks = fails = 0

def check(name, cond):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL functional_server:", name)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def call(port, method, path, body=None, timeout=30):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=body, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())

def q(port, sql):
    return call(port, "POST", "/query", sql.encode())

def start(d, port, threads=8, queue=256):
    p = subprocess.Popen([os.path.join(ROOT, "minidb-server"), "-d", d, "-p", str(port), "-t", str(threads), "-q", str(queue)],
                         stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            if call(port, "GET", "/health", timeout=1)[0] == 200: return p
        except Exception: time.sleep(0.05)
    p.kill(); raise SystemExit("server did not start")

def raw(port, data, read=True):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(data)
    out = b""
    while read:
        chunk = s.recv(65536)
        if not chunk: break
        out += chunk
    s.close()
    return out

d = tempfile.mkdtemp()
with open(os.path.join(d, "users.schema"), "w") as f: f.write("id:int,name:string,age:int\n")
port = free_port()
srv = start(d, port)
try:
    # ── 기본 동작 ──
    st, b = call(port, "GET", "/health");                       check("health", st == 200 and b == {"status": "ok"})
    st, b = q(port, "INSERT INTO users VALUES ('kim', 25);")
    check("insert", st == 200 and b["ok"] and b["results"][0]["first_id"] == 1)
    st, b = q(port, "INSERT INTO users VALUES ('a \"q\" \\\\ ,\n한글', 1); SELECT * FROM users WHERE id = 2;")
    check("multi statement + escaping", st == 200 and b["results"][1]["rows"] == [[2, 'a "q" \\\\ ,\n한글', 1]])
    st, b = q(port, "SELECT * FROM users WHERE id = 1;")
    check("select by id uses index", b["results"][0]["access"] == "index" and b["results"][0]["rows"] == [[1, "kim", 25]])
    st, b = q(port, "SELECT name FROM users WHERE age = 25;")
    check("select by field uses scan", b["results"][0]["access"] == "scan" and b["results"][0]["rows"] == [["kim"]])
    st, b = q(port, "SELECT * FROM users WHERE id = 12345;")
    check("no match is 200 with empty rows", st == 200 and b["results"][0]["row_count"] == 0)

    # ── 오류 응답 ──
    st, b = q(port, "SELEC 1;");                                check("syntax error → 400", st == 400 and not b["ok"] and "error" in b["results"][0])
    st, b = q(port, "SELECT * FROM ghost;");                    check("unknown table → 400", st == 400 and "does not exist" in b["results"][0]["error"])
    st, b = q(port, "SELECT * FROM users; BAD;")
    check("partial failure → 400 but first result kept", st == 400 and b["results"][0]["ok"] and not b["results"][1]["ok"])
    st, b = q(port, "");                                        check("empty body → 400", st == 400)
    st, b = q(port, "   -- just a comment\n");                  check("comment only → 400", st == 400)
    st, b = call(port, "GET", "/query");                        check("GET /query → 405", st == 405)
    st, b = call(port, "POST", "/health", b"x");                check("POST /health → 405", st == 405)
    st, b = call(port, "GET", "/nope");                         check("unknown path → 404", st == 404)
    r = raw(port, b"POST /query HTTP/1.1\r\nHost: x\r\n\r\n");  check("no Content-Length → 411", r.startswith(b"HTTP/1.1 411"))
    r = raw(port, b"POST /query HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"); check("huge body → 413", r.startswith(b"HTTP/1.1 413"))
    r = raw(port, b"POST /query HTTP/1.1\r\nContent-Length: abc\r\n\r\n");         check("bad Content-Length → 400", r.startswith(b"HTTP/1.1 400"))
    r = raw(port, b"garbage\r\n\r\n");                          check("malformed request → 400", r.startswith(b"HTTP/1.1 400"))
    r = raw(port, b"POST /query HTTP/1.1\r\nContent-Length: 50\r\n\r\nSELECT");
    check("short body does not hang forever", r.startswith(b"HTTP/1.1 408") or r.startswith(b"HTTP/1.1 400"))
    r = raw(port, b"POST /query HTTP/1.1\r\ncontent-length: 20\r\nExpect: 100-continue\r\n\r\nSELECT * FROM users;")
    check("Expect: 100-continue + lowercase header", r.startswith(b"HTTP/1.1 100") and b"HTTP/1.1 200" in r)
    raw(port, b"", read=False)                                  # 연결만 하고 끊는 클라이언트
    st, b = call(port, "GET", "/health");                       check("alive after bad clients", st == 200)

    # ── 동시성: 32개 클라이언트가 동시에 INSERT 50번 + SELECT ──
    N, M = 32, 50
    ids, errs, lock = [], [], threading.Lock()
    def client(k):
        for i in range(M):
            try:
                st, b = q(port, f"INSERT INTO users VALUES ('c{k}_{i}', {k});")
                st2, b2 = q(port, f"SELECT name FROM users WHERE id = {b['results'][0]['first_id']};")
                with lock:
                    ids.append(b["results"][0]["first_id"])
                    if st != 200 or st2 != 200 or b2["results"][0]["rows"] != [[f"c{k}_{i}"]]: errs.append((k, i))
            except Exception as e:
                with lock: errs.append(repr(e))
    ts = [threading.Thread(target=client, args=(k,)) for k in range(N)]
    for t in ts: t.start()
    for t in ts: t.join()
    check("concurrent requests all succeeded", not errs)
    check("ids unique and contiguous", sorted(ids) == list(range(3, 3 + N * M)))
    st, b = q(port, "SELECT id FROM users;");                   check("row count after concurrency", b["results"][0]["row_count"] == 2 + N * M)
    st, b = q(port, "SELECT id FROM users WHERE age = 7;");     check("scan finds one client's rows", b["results"][0]["row_count"] == M)

    # ── 종료 후 재시작: 데이터 유지 ──
    srv.send_signal(signal.SIGINT); check("graceful shutdown", srv.wait(timeout=10) == 0)
    srv = start(d, port)
    st, b = q(port, "SELECT name FROM users WHERE id = 1;");    check("data survives restart", b["results"][0]["rows"] == [["kim"]])
    srv.send_signal(signal.SIGINT); srv.wait(timeout=10)

    # ── 과부하: 워커 1개, 큐 1칸 → 초과 연결은 503 ──
    srv = start(d, port, threads=1, queue=1)
    hold = [socket.create_connection(("127.0.0.1", port)) for _ in range(6)]
    time.sleep(0.3)

    got503 = False
    for h in hold[2:]:
        h.settimeout(1)
        try:
            if h.recv(64).startswith(b"HTTP/1.1 503"):
                got503 = True
                break
        except socket.timeout:
            continue

    check("overload → 503", got503)
    for h in hold:
        h.close()

finally:
    srv.kill()
    srv.wait()
    shutil.rmtree(d, ignore_errors=True)

print("%-28s %5d checks, %d failed" % (
    "tests/functional_server.py", checks, fails
))
sys.exit(1 if fails else 0)
