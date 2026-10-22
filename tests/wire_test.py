#!/usr/bin/env python3
"""Wire-protocol test: spawns the server and talks to it with a minimal MySQL client."""
import hashlib, os, signal, socket, struct, subprocess, sys, time

PORT = 33907
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def rd(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b

def read_packet(s):
    h = rd(s, 4)
    return h[3], rd(s, h[0] | h[1] << 8 | h[2] << 16)

def send_packet(s, seq, payload):
    s.sendall(struct.pack("<I", len(payload))[:3] + bytes([seq]) + payload)

def lenenc(b, i):
    c = b[i]
    if c < 251: return c, i + 1
    if c == 0xfb: return None, i + 1
    if c == 0xfc: return struct.unpack_from("<H", b, i + 1)[0], i + 3
    if c == 0xfd: return int.from_bytes(b[i + 1:i + 4], "little"), i + 4
    return struct.unpack_from("<Q", b, i + 1)[0], i + 9

def lstr(b, i):
    n, i = lenenc(b, i)
    return (None, i) if n is None else (b[i:i + n].decode(), i + n)

def connect(user, password, db=None):
    s = socket.create_connection(("127.0.0.1", PORT))
    seq, hs = read_packet(s)
    assert hs[0] == 10
    i = hs.index(b"\0", 1) + 1 + 4
    part1 = hs[i:i + 8]
    i += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10
    scramble = part1 + hs[i:i + 12]
    token = b""
    if password:
        s1 = hashlib.sha1(password.encode()).digest()
        t = hashlib.sha1(scramble + hashlib.sha1(s1).digest()).digest()
        token = bytes(a ^ b for a, b in zip(s1, t))
    caps = 0x1 | 0x4 | 0x200 | 0x2000 | 0x8000 | 0x80000 | (0x8 if db else 0)
    r = struct.pack("<IIB", caps, 1 << 24, 45) + b"\0" * 23 +user.encode() + b"\0" + bytes([len(token)]) + token
    if db: r += db.encode() + b"\0"
    r += b"mysql_native_password\0"
    send_packet(s, seq + 1, r)
    _, p = read_packet(s)
    return s, p

def cmd(s, code, text=""):
    send_packet(s, 0, bytes([code]) + text.encode())
    _, p = read_packet(s)
    if p[0] == 0x00:
        aff, i = lenenc(p, 1); last, i = lenenc(p, i)
        return ("ok", aff, last)
    if p[0] == 0xff:
        return ("err", struct.unpack_from("<H", p, 1)[0], p[9:].decode(), p[4:9].decode())
    ncols, _ = lenenc(p, 0)
    assert ncols is not None
    names = []
    for _ in range(ncols):
        _, d = read_packet(s)
        i = 0
        for k in range(5): _, i = lstr(d, i)
        name, i = lstr(d, i)
        names.append(name)
    _, e = read_packet(s); assert e[0] == 0xfe
    rows = []
    while True:
        _, p = read_packet(s)
        if p[0] == 0xfe and len(p) < 9: break
        row, i = [], 0
        for _ in range(ncols):
            v, i = lstr(p, i); row.append(v)
        rows.append(row)
    return ("rows", names, rows)

def q(s, sql): return cmd(s, 0x03, sql)

def lestr(v):
    b = v.encode()
    assert len(b) < 251
    return bytes([len(b)]) + b

def prepare(s, sql):
    send_packet(s, 0, b"\x16" + sql.encode())
    _, p = read_packet(s)
    if p[0] == 0xff: return ("err", struct.unpack_from("<H", p, 1)[0])
    sid, ncols, nparams = struct.unpack_from("<IHH", p, 1)
    if nparams:
        for _ in range(nparams): read_packet(s)
        read_packet(s)
    names = []
    if ncols:
        for _ in range(ncols):
            _, d = read_packet(s)
            i = 0
            for k in range(4): _, i = lstr(d, i)
            n, i = lstr(d, i); names.append(n)
        read_packet(s)
    return ("ok", sid, ncols, nparams, names)

def execute(s, sid, params):
    n = len(params)
    nb = bytearray((n + 7) // 8)
    types, vals = b"", b""
    for i, v in enumerate(params):
        if v is None: nb[i // 8] |= 1 << (i % 8); types += b"\x06\x00"
        elif isinstance(v, int): types += b"\x08\x00"; vals += struct.pack("<q", v)
        elif isinstance(v, float): types += b"\x05\x00"; vals += struct.pack("<d", v)
        else: types += b"\xfd\x00"; vals += lestr(v)
    body = b"\x17" + struct.pack("<IBI", sid, 0, 1)
    if n: body += bytes(nb) + b"\x01" + types + vals
    send_packet(s, 0, body)
    _, p = read_packet(s)
    if p[0] == 0x00:
        aff, i = lenenc(p, 1); last, i = lenenc(p, i)
        return ("ok", aff, last)
    if p[0] == 0xff: return ("err", struct.unpack_from("<H", p, 1)[0], p[9:].decode())
    ncols, _ = lenenc(p, 0)
    assert ncols is not None  # Fixes Pylance type error for None + int
    ctypes = []
    for _ in range(ncols):
        _, d = read_packet(s)
        i = 0
        for k in range(6): _, i = lstr(d, i)
        ctypes.append(d[i + 1 + 2 + 4])
    read_packet(s)
    rows = []
    while True:
        _, p = read_packet(s)
        if p[0] == 0xfe and len(p) < 9: break
        assert p[0] == 0x00
        nbm = (ncols + 9) // 8
        bm = p[1:1 + nbm]; i = 1 + nbm; row = []
        for c, t in enumerate(ctypes):
            if bm[(c + 2) // 8] >> ((c + 2) % 8) & 1: row.append(None); continue
            if t == 0x08: row.append(struct.unpack_from("<q", p, i)[0]); i += 8
            elif t == 0x05: row.append(struct.unpack_from("<d", p, i)[0]); i += 8
            else: v, i = lstr(p, i); row.append(v)
        rows.append(row)
    return ("rows", rows)

def main():
    srv = subprocess.Popen([os.path.join(ROOT, "mysqlclone"), "--memory", "--listen", str(PORT), "--user", "root", "--password", "secret"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", PORT)).close(); break
            except OSError: time.sleep(0.05)
        s, p = connect("root", "wrong")
        assert p[0] == 0xff and struct.unpack_from("<H", p, 1)[0] == 1045, "bad password must be rejected"
        s.close()
        s, p = connect("root", "secret")
        assert p[0] == 0x00, "login failed"
        assert q(s, "CREATE DATABASE w")[0] == "ok"
        assert cmd(s, 0x02, "w")[0] == "ok"
        assert q(s, "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(9))")[0] == "ok"
        assert q(s, "INSERT INTO t (v) VALUES ('a'),(NULL)") == ("ok", 2, 1)
        assert q(s, "SELECT id, v FROM t ORDER BY id") == ("rows", ["id", "v"], [["1", "a"], ["2", None]])

        r = q(s, "SELECT * FROM nope")
        assert r[0] == "err" and r[1] == 1146 and r[3] == "42S02", r  # type: ignore

        assert q(s, "select @@version_comment limit 1") == ("rows", ["@@version_comment"], [["mysqlclone"]])
        assert q(s, "SET NAMES utf8mb4")[0] == "ok"
        variables = q(s, "SHOW VARIABLES LIKE 'version%'")[2]
        assert isinstance(variables, list) and len(variables) == 2
        assert cmd(s, 0x0e)[0] == "ok"
        assert q(s, "SELECT 1; SELECT 2") == ("rows", ["2"], [["2"]])
        # --- prepared statements (binary protocol) ---
        pi = prepare(s, "INSERT INTO t (v) VALUES (?)")
        assert pi[0] == "ok" and pi[2] == 0 and pi[3] == 1, pi
        assert execute(s, pi[1], ["a'b\\c"]) == ("ok", 1, 3)          # quote + backslash survive binding
        assert execute(s, pi[1], [None])[0:2] == ("ok", 1)
        assert q(s, "SELECT v FROM t WHERE id = 3") == ("rows", ["v"], [["a'b\\c"]])
        assert execute(s, pi[1], ["x", "y"])[0] == "err"               # wrong parameter count
        ps = prepare(s, "SELECT id, v FROM t WHERE id >= ? ORDER BY id")
        assert ps[0] == "ok" and ps[2:] == (2, 1, ["id", "v"]), ps
        assert execute(s, ps[1], [2]) == ("rows", [[2, None], [3, "a'b\\c"], [4, None]])
        assert execute(s, ps[1], [100]) == ("rows", [])
        pd = prepare(s, "SELECT ? + 1, ?")
        assert pd[3] == 2  # type: ignore
        r = execute(s, pd[1], [2.5, "hi"])
        assert r[0] == "rows" and float(r[1][0][0]) == 3.5 and r[1][0][1] == "hi", r
        assert prepare(s, "SELEKT ?")[0] == "err"
        send_packet(s, 0, b"\x19" + struct.pack("<I", pi[1]))            # COM_STMT_CLOSE (no reply)
        assert execute(s, pi[1], ["z"])[0] == "err"                    # closed handle
        assert q(s, "DELETE FROM t WHERE id > 2")[0] == "ok"
        assert q(s, "BEGIN")[0] == "ok" and q(s, "INSERT INTO t (v) VALUES ('unc')")[1] == 1
        s.close()  # disconnect with an open transaction -> rolled back
        s, p = connect("root", "secret", db="w")
        assert p[0] == 0x00
        assert q(s, "SELECT COUNT(*) FROM t") == ("rows", ["COUNT(*)"], [["2"]])
        send_packet(s, 0, b"\x01"); s.close()
        srv.send_signal(signal.SIGTERM)
        assert srv.wait(timeout=5) == 0, "server must exit cleanly on SIGTERM"
        print("PASS (wire)")
    except Exception:
        srv.kill(); print(srv.stderr.read().decode() if srv.stderr else ""); print("FAIL (wire)"); raise

main()