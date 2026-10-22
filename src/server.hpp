#pragma once
#include <string>

#include "executor.hpp"

struct ServerOptions {
    std::string bind = "127.0.0.1";
    int port = 3307;
    std::string user, password; // both empty = accept any credentials
};

// MySQL client/server protocol (v10 handshake, mysql_native_password, text protocol).
// Connections are served one at a time (the engine is single-session). Returns on SIGINT/SIGTERM.
int serve(Engine& eng, const ServerOptions& opt);