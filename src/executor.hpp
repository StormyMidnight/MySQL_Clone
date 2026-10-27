#pragma once
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <set>
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
    mutable const Table* insTable = nullptr;  // INSERT ... ON DUPLICATE KEY UPDATE: row being inserted, for VALUES(col)
    mutable const Row* insRow = nullptr;
    mutable const std::vector<std::pair<const Expr*, Value>>* curWin = nullptr;  // window function values for the row being emitted
    std::map<std::string, Value>* vars = nullptr;  // user variables (@x), session scoped
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
    std::map<std::string, Value> vars_;
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
    std::map<std::string, std::shared_ptr<Stmt>> checkCache_;  // parsed CHECK conditions, by text
    void checkRow(const Table& t, const Row& r);
    void computeGenerated(const Table& t, Row& r, size_t rowNo);  // fills GENERATED columns from the other values                // throws 3819 when a CHECK evaluates to false
    Table infoSchema(const std::string& name);                  // INFORMATION_SCHEMA virtual tables
    // ---- foreign keys ----
    struct FkChild { Table* t; std::string db; size_t fk; };
    std::map<Table*, std::vector<Row>> fkUndo_;  // pre-statement rows of other tables touched by cascades
    std::vector<FkChild> fkChildren(const std::string& db, const std::string& parent);
    std::string fkClause(const std::string& db, const Table& child, const ForeignKey& fk, bool show) const;
    ForeignKey makeFk(const FkDef& d, Table& child, const std::string& db, size_t ordinal);
    void fkCheckChild(const Table& t, const std::string& db, const Row& r);
    void fkOnParentUpdate(const std::string& db, Table& parent, const Row& oldRow, const Row& newRow, int depth);
    void fkPlanDelete(const std::string& db, Table& t, std::set<size_t>& rows);
    void fkBackup(Table* t);
    void fkRestore();
    std::vector<std::pair<std::string, Table*>> cteStack_;  // visible CTEs, innermost last
    const Table* tableFor(const TableRef& r, std::deque<Table>& temps);  // real table / view / CTE / derived table
    Result selectChain(const SelectStmt& s, const void* outer);          // SELECT [UNION ...] without WITH handling
    Table cteTable(const std::string& name, const std::vector<std::string>& cols, Result& res);
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