# MySQL Clone
A high-performance from-scratch C++17 MySQL-dialect SQL engine and interactive command-line interface. Engineered without external dependencies beyond the C++ STL (plus POSIX `isatty`), compiling warning-free under strict compilation flags (`-Wall -Wextra -Wpedantic -Werror`).

## Quick Start & Build
Requires a modern C++17 compiler and `make`.
```bash
make        # Builds the ./mysqlclone executable
make test   # Runs tests/smoke.sql against tests/expected.txt
```

### CLI Modes
```bash
./mysqlclone                         # Interactive monitor (persists data in ./mysqlclone.data)
./mysqlclone --memory -t < x.sql     # In-memory execution with tabular output
./mysqlclone -D db -e "SELECT 1"     # One-shot execution (tab-separated unless -t specified)
```

### Server Mode (MySQL Wire Protocol)
Spawns a native MySQL-compatible server socket so standard database clients and connectors can connect directly:
```bash
./mysqlclone --listen 3307 [--bind 127.0.0.1] [--user root --password secret]
mysql -h127.0.0.1 -P3307 -uroot -psecret
```
*Supports the v10 handshake, `mysql_native_password` authentication, `COM_QUERY`, `INIT_DB`, `PING`, `QUIT`, text-protocol result sets, and binary protocol prepared statements (`COM_STMT_PREPARE`, `EXECUTE`, `SEND_LONG_DATA`, `RESET`, `CLOSE`).*
