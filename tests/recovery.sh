#!/bin/sh
# Crash-recovery test: committed work survives a crash, uncommitted work and a torn WAL tail do not.
cd "$(dirname "$0")/.." || exit 1
D=/tmp/mysqlclone_rec.data
rm -f "$0" "$D.wal" "$D.tmp"
./mysqlclone --data "$D" --crash-test -e "CREATE DATABASE r; USE r; CREATE TABLE t(id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(9), ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP);
INSERT INTO t(v) VALUES ('a'),('b'); BEGIN; INSERT INTO t(v) VALUES ('c'); COMMIT;
BEGIN; INSERT INTO t(v) VALUES ('lost'); UPDATE t SET v='x' WHERE id=1;" || exit 1
[ -s "$D.wal" ] || { echo "FAIL: no WAL after crash"; exit 1; }
printf 'G 999 50 abc\nxx' >> "$D.wal"   # torn tail
./mysqlclone --data "$D" --crash-test -D r -e "INSERT INTO t(v) VALUES ('d');" || exit 1
./mysqlclone --data "$D" -t -D r -e "SELECT id, v, ts IS NOT NULL AS has_ts FROM t ORDER BY id" | sed 's/([0-9.]* sec)/(X sec)/' > /tmp/rec.out
cat > /tmp/rec.exp <<'EOT'
Database changed

+----+---+--------+
| id | v | has_ts |
+----+---+--------+
|  1 | a |      1 |
|  2 | b |      1 |
|  3 | c |      1 |
|  4 | d |      1 |
+----+---+--------+
4 rows in set (X sec)

EOT
if diff -u /tmp/rec.exp /tmp/rec.out && [ ! -s "$D.wal" ]; then echo "PASS (recovery)"; else echo "FAIL (recovery)"; exit 1; fi
