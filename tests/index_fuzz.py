#!/usr/bin/env python3
"""Differential test: the same random statements run against an indexed table and an unindexed twin
must return identical results (checks the index planner, incremental index maintenance, rollback)."""
import os, random, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rnd = random.Random(12345)
words = ["a", "B", "b", "cc", "Dd", "e", "zz", "Q"]

def rv(n): return "NULL" if rnd.random() < 0.1 else str(rnd.randint(0, n))
def rs(): return "NULL" if rnd.random() < 0.1 else "'%s'" % rnd.choice(words)

def pred():
    k = rnd.choice(["a", "a", "b", "c", "d"])
    if k == "a":
        op = rnd.choice(["=", "<", "<=", ">", ">="]); return "a %s %d" % (op, rnd.randint(-1, 21))
    if k == "b":
        return rnd.choice(["b = '%s'", "b >= '%s'", "b < '%s'"]) % rnd.choice(words + ["bb"])
    if k == "c":
        lo = rnd.randint(0, 15); return "c BETWEEN %d AND %d" % (lo, lo + rnd.randint(0, 8))
    return "a IN (%s)" % ", ".join(str(rnd.randint(0, 20)) for _ in range(rnd.randint(1, 4)))

def where():
    n = rnd.choice([1, 1, 2, 3])
    ps = [pred() for _ in range(n)]
    if rnd.random() < 0.15: ps.append("(" + pred() + " OR " + pred() + ")")
    return " AND ".join(ps)

def stmts(t):
    out = []
    next_id = 1
    for _ in range(400):
        r = rnd.random()
        if r < 0.30:
            rows = []
            for _ in range(rnd.randint(1, 4)):
                rows.append("(%d, %s, %s, %s, %s)" % (next_id, rv(20), rs(), rv(15), rv(5))); next_id += 1
            out.append("INSERT INTO %s VALUES %s;" % (t, ", ".join(rows)))
        elif r < 0.40:
            out.append("UPDATE %s SET a = %s, b = %s WHERE %s;" % (t, rv(20), rs(), where()))
        elif r < 0.45:
            out.append("UPDATE %s SET c = c + 1 WHERE %s;" % (t, where()))
        elif r < 0.50:
            out.append("DELETE FROM %s WHERE %s;" % (t, where()))
        elif r < 0.52:
            out.append(rnd.choice(["BEGIN;", "ROLLBACK;", "COMMIT;"]))
        elif r < 0.54:
            out.append("INSERT INTO %s VALUES (%d, 1, 'x', 1, 1);" % (t, rnd.randint(1, max(1, next_id))))  # maybe dup PK
        else:
            out.append("SELECT id FROM %s WHERE %s ORDER BY id;" % (t, where()))
    out.append("SELECT id, a, b, c, d FROM %s ORDER BY id;" % t)
    return out

def run(script):
    p = subprocess.run([os.path.join(ROOT, "mysqlclone"), "--memory", "-f"], input=script, capture_output=True, text=True, timeout=120)
    return p.stdout + p.stderr

ddl = ("CREATE DATABASE f; USE f;\n"
       "CREATE TABLE t_idx (id INT PRIMARY KEY, a INT, b VARCHAR(8), c INT, d INT, KEY ia (a), KEY ib (b), KEY iac (a, c), KEY ic (c));\n"
       "CREATE TABLE t_raw (id INT, a INT, b VARCHAR(8), c INT, d INT);\n")
rnd.seed(777)
s1 = stmts("t_idx")
rnd.seed(777)
s2 = stmts("t_raw")
# the raw twin has no primary key, so skip its duplicate-PK probes by filtering the same statements out of both
def drop_dup(a, b):
    keep = [i for i, x in enumerate(a) if not (x.startswith("INSERT INTO") and x.endswith("'x', 1, 1);"))]
    return [a[i] for i in keep], [b[i] for i in keep]
s1, s2 = drop_dup(s1, s2)
o1 = run(ddl + "\n".join(s1) + "\n").replace("t_idx", "T")
o2 = run(ddl + "\n".join(s2) + "\n").replace("t_raw", "T")
if o1 != o2:
    a, b = o1.splitlines(), o2.splitlines()
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y: print("first difference at line", i, "\n  indexed:", x, "\n  raw    :", y); break
    print("FAIL (index fuzz)"); sys.exit(1)
print("PASS (index fuzz, %d statements)" % len(s1))