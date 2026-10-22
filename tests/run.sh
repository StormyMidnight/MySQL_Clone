#!/bin/sh
# Runs the smoke script and compares against tests/expected.txt(timings normalized).
cd "$(dirname "$0")/.." || exit 1
./mysqlclone --memory -t -f < tests/smoke.sql 2>&1 | sed 's/([0-9.]* sec)/(X sec)/' > /tmp/mysqlclone.out
if diff -u tests/expected.txt /tmp/mysqlclone.out; then echo "PASS"; else echo "FAIL"; exit 1; fi
