# mysqlclone

From-scratch C++17 MySQL-dialect SQL engine and CLI. No dependencies beyond the STL (+ POSIX `isatty`). Builds warning-free with `-Wall -Wextra -Wpedantic -Werror`.

    make            # builds ./mysqlclone
    make test       # runs tests/smoke.sql against tests/expected.txt
    ./mysqlclone                     # interactive monitor (data in ./mysqlclone.data)
    ./mysqlclone --memory -t < x.sql # in-memory, table output
    ./mysqlclone -D db -e "SELECT 1" # one-shot (tab-separated unless -t)

Server mode (MySQL wire protocol, so `mysql`/connectors can connect):

    ./mysqlclone --listen 3307 [--bind 127.0.0.1] [--user root --password secret]
    mysql -h127.0.0.1 -P3307 -uroot -psecret

Supports the v10 handshake, `mysql_native_password` (verified if `--user/--password` given, otherwise any credentials are accepted — keep it bound to localhost), COM_QUERY/INIT_DB/PING/QUIT, text-protocol result sets, prepared statements over the binary protocol (COM_STMT_PREPARE/EXECUTE/SEND_LONG_DATA/RESET/CLOSE; `?` placeholders incl. `LIMIT ?`; parameters are bound as escaped SQL literals so the WAL stays replayable; verified with a raw-protocol test client, not yet with real drivers), OK/ERR packets with MySQL codes, `@@system` variables, SHOW VARIABLES/WARNINGS. Clients are served one at a time; an open transaction is rolled back on disconnect. Not supported: server-side cursors (COM_STMT_FETCH), TLS, compression.

Flags: `-e SQL`, `-t` table output, `-f` keep going after errors in scripts, `-D db`, `--data FILE`, `--memory`, `--listen PORT`, `--bind`, `--user`, `--password`.

## Supported
- DDL: CREATE/DROP DATABASE, CREATE/DROP/TRUNCATE TABLE (IF [NOT] EXISTS), column types: INT family (TINYINT..BIGINT, `UNSIGNED`, range-checked, error 1264), FLOAT/DOUBLE (floating point) and DECIMAL(p,s)/NUMERIC (rounded to scale, range-checked, printed with exactly `s` places; backed by a double, so exact up to ~15 digits), CHAR/VARCHAR/TEXT/BLOB (length-checked), Oracle-style aliases VARCHAR2/NVARCHAR2/NVARCHAR/NCHAR (`VARCHAR2(10 CHAR)` ok), DATE/DATETIME/TIMESTAMP (validated and normalized to ISO text; also accepts YYYYMMDD ints; error 1292), ENUM (unchecked); NOT NULL, DEFAULT (incl. CURRENT_TIMESTAMP), AUTO_INCREMENT, PRIMARY KEY (column or composite), UNIQUE; other index/FK clauses are parsed and ignored.
- DML: INSERT (multi-row, column list, DEFAULT, INSERT ... SELECT), UPDATE / DELETE with WHERE, ORDER BY, LIMIT.
- SELECT: expressions, aliases, `*`, `t.*`, DISTINCT, INNER/LEFT/RIGHT/CROSS/comma joins, WHERE, GROUP BY (column/alias/ordinal), HAVING (aliases ok), ORDER BY (expr/alias/ordinal), LIMIT n / n,m / OFFSET, CASE, IN, BETWEEN, LIKE, IS NULL, `<=>`, three-valued logic.
- Aggregates: COUNT (incl. DISTINCT, *), SUM, AVG, MIN, MAX, GROUP_CONCAT.
- Date functions: DATE, DATEDIFF, DATE_ADD/DATE_SUB/ADDDATE/SUBDATE (`INTERVAL n SECOND..YEAR`, also `date + INTERVAL 1 DAY`), DAYOFWEEK, HOUR/MINUTE/SECOND, YEAR/MONTH/DAY. DATE/DATETIME columns compared (=, <, BETWEEN, IN ...) with loosely formatted literals like `'2024-1-5'` are normalized first.
- Numeric typing follows MySQL: INT/DECIMAL arithmetic yields DECIMAL (`10/4` = `2.5000`, `AVG(int)` = `2.0000`, `2*1.50` = `3.00`), decimal literals are exact decimals, anything involving DOUBLE or text is floating point.
- Functions: CONCAT, UPPER/LOWER, LENGTH, CHAR_LENGTH, SUBSTRING, TRIM/LTRIM/RTRIM, REPLACE, LEFT/RIGHT, REVERSE, REPEAT, ABS, ROUND, FLOOR, CEIL, MOD, POW, SQRT, COALESCE, IFNULL, NULLIF, IF, GREATEST, LEAST, NOW, CURDATE, YEAR/MONTH/DAY, DATABASE, VERSION, LAST_INSERT_ID.
- Subqueries (scalar, IN, EXISTS; correlated subqueries supported to any nesting depth — uncorrelated ones are evaluated once and cached), UNION / UNION ALL, ALTER TABLE (ADD [COLUMN] / DROP [COLUMN] / RENAME TO / ADD UNIQUE), CREATE [UNIQUE] INDEX, DROP INDEX (non-unique indexes are accepted but not stored).
- Views: `CREATE [OR REPLACE] VIEW v [(cols)] AS SELECT ...`, `DROP VIEW [IF EXISTS]`; views are read-only (error 1288 on DML), may reference other views (recursion guard), appear in `SHOW TABLES`/`DESCRIBE`, and persist (snapshot v4 / WAL). A view is materialized per query (no predicate pushdown).
- ALTER TABLE also supports `MODIFY` / `CHANGE` (rows converted and re-validated; all-or-nothing), `RENAME COLUMN a TO b`, `ADD/DROP PRIMARY KEY`, `ADD INDEX`, `DROP INDEX`. A trailing `ORDER BY` / `LIMIT` applies to the whole UNION.
- SHOW DATABASES/TABLES/COLUMNS/CREATE TABLE, DESCRIBE, USE.
- BEGIN/START TRANSACTION/COMMIT/ROLLBACK (snapshot-based; DDL implicitly commits). `SET ...` is accepted and ignored.
- Ordered (B-tree-style, `std::map`) indexes: PRIMARY/UNIQUE/`KEY`/`CREATE INDEX`/`ALTER ... ADD INDEX`, composite keys, persisted in the snapshot (v3; v1/v2 files still load). The planner turns WHERE conjuncts (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN (...)`, constants on either side, DATE literals normalized) into point/prefix/range scans, also for UPDATE/DELETE and correlated subqueries (outer columns count as constants); joins probe an index on the right table when one exists. `EXPLAIN SELECT ...` shows type/key/rows. Uniqueness checks use the index (O(log n)). Rows with NULL in an indexed column are not indexed. Indexes are derived data, maintained incrementally on INSERT/UPDATE and rebuilt lazily after DELETE/ALTER/rollback. 100k-row table: point lookup, range scan, 500x100k indexed join and correlated subquery all run in a few ms each.
- Hash join for equi-joins (`a.x = b.y`, same-kind types), hash-indexed uniqueness checks during INSERT (4k x 4k join in ~50 ms).
- MySQL-style errors (1062 duplicate entry, 1146, 1054, 1064, ...), MySQL-style table output and timing lines.
- Durability: write-ahead log (`<data>.wal`, checksummed groups, fsync per commit) + atomic snapshot checkpoints (every 500 commits and on clean exit). Startup loads the snapshot and replays the WAL; torn tails are discarded, uncommitted transactions are never logged. Statement timestamps are logged so NOW() replays deterministically. `--crash-test` exits without a checkpoint to simulate a crash.

## Not yet (honest limits)
a cost-based join planner (joins use an index probe or hash join for equi-joins, nested loop otherwise; no join reordering), index-ordered ORDER BY/covering scans, indexes on prefixes/expressions, foreign keys, `ALTER ... FIRST/AFTER`, updatable views, arbitrary-precision DECIMAL (>15 digits), case-sensitivity options, MVCC/concurrency (single-user; WAL is logical, statement-level), concurrent sessions. Single-session, whole-snapshot persistence.

## Layout
`src/value.hpp` values/errors · `lexer` · `ast.hpp` · `parser` · `storage` (catalog + snapshot file) · `executor` (eval, planner-free executor) · `wal` (write-ahead log) · `server` (wire protocol) · `main.cpp` (shell).