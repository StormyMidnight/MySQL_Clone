#pragma once
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "storage.hpp"
#include "wal.hpp"

struct Result {
    bool isSet = false, silent = false, timing = true;
    std::vector<std::string> headers;
    std::vector<Row> rows;
    std::string msg, info;
};

struct Env {
    std::string db;
    int64_t lastId = 0;
    std::time_t now = 0;  // statement timestamp (fixed during WAL replay)
    std::function<std::vector<Row>(const SelectStmt&, const void* outer)> sub; // runs a subquery; outer = enclosing eval context
    mutable int outerRefs = 0; //bumped when a column resolves in an enclosing query (detects correlation)
};

struct LogEntry { int64_t time = 0; std::string db, sql; };

class Engine {
public:
    explicit Engine(std::string dataFile);  // empty path = in-memory only; recovers a snapshot + WAL
    ~Engine();                              // rolls back an open txn, checkpoints
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Result execute(const std::string& sql);
    const std::string& currentDb() const { return env_.db; }
    int64_t lastInsertId() const { return env_.lastId; }
    bool inTransaction() const { return inTxn_; }
    std::vector<std::string> resultColumns(const std::string& sql); // column names a SELECT would return (empty otherwise)
    void resetSession();    // roll back an open txn, clear current db (new client connection)

private:
    Catalog cat_, snapshot_;
    bool inTxn_ = false;
    std::string file_;
    Wal wal_;
    Env env_;
    uint64_t lsn_ = 0;
    size_t walGroups_ = 0;
    bool replaying_ = false;
    std::time_t replayTime_ = 0;
    std::vector<LogEntry> pending_;

    Database& database(const std::string& name);
    Table& table(const std::string& db, const std::string& name);  // real tables only (views -> error 1288)
    // Table or view for reading; a view is materialized into `temps`.
    const Table* relation(const std::string& db, const std::string& name, std::deque<Table>& temps);
    int viewDepth_ = 0;
    void flushLog();    // append pending committed statements to the WAL (fsync)
    void checkpoint();  // snapshot + truncate WAL
    Result select(const SelectStmt& s, const void* outer = nullptr);  // handles UNION chains
    Result selectOne(const SelectStmt& s, const void* outer);         // a single SELECT
    Result alter(Stmt& s);
    Result explain(const SelectStmt& s);
    Result createTable(Stmt& s);
    Result insert(Stmt& s);
    Result update(Stmt& s);
    Result del(Stmt& s);
    Result describe(const Stmt& s);
    Result showCreate(const Stmt& s);
};