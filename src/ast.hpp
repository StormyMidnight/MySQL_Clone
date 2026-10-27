#pragma once
#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

struct Expr;
struct SelectStmt;
struct WinSpec;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
	enum class K { Lit, Col, Unary, Bin, Func, IsNull, In, Between, Like, Case, Sub, Exists, Param };
	K k = K::Lit;
	Value v;
	std::string table, name;  // Col: table/name; Unary/Bin/Func: name = operator/function
	std::vector<ExprP> args;
	bool neg = false, distinct = false, star = false, hasOperand = false, hasElse = false;
	mutable int slot = -1;        // cached column index ...
	mutable int level = 0;        // ... found this many enclosing query levels up (correlated subqueries)
	mutable bool correlated = false;  // subquery depends on outer rows -> never cache its result
	std::shared_ptr<WinSpec> win;     // Func ... OVER (...)
	std::shared_ptr<SelectStmt> sub;  // Sub / Exists / IN (SELECT ...)
	mutable std::shared_ptr<std::vector<std::vector<Value>>> cache;  // uncorrelated subquery result
};

struct SelItem { ExprP e; std::string alias, text, starTable; bool star = false; };
struct TableRef { std::string db, name, alias; std::shared_ptr<SelectStmt> derived; };  // derived: FROM (SELECT ...) alias
struct JoinClause {
	enum class Type { Inner, Left, Right, Cross } type = Type::Inner;
	TableRef ref;
	ExprP on;
};
struct OrderItem { ExprP e; bool desc = false; };
struct FrameBound { enum class K { UnboundedPreceding, Preceding, CurrentRow, Following, UnboundedFollowing } k = K::CurrentRow; int64_t n = 0; };
struct WinSpec {
	std::vector<ExprP> part;
	std::vector<OrderItem> order;
	bool hasFrame = false, rows = true;  // ROWS vs RANGE
	FrameBound lo{ FrameBound::K::UnboundedPreceding, 0 }, hi{ FrameBound::K::CurrentRow, 0 };
};
struct CteDef { std::string name; std::vector<std::string> cols; std::shared_ptr<SelectStmt> sel; };
struct SelectStmt {
	std::vector<CteDef> ctes;  // WITH [RECURSIVE] ... (head of a statement only)
	bool recursive = false;
	bool distinct = false;
	std::vector<SelItem> items;
	bool hasFrom = false;
	TableRef from;
	std::vector<JoinClause> joins;
	ExprP where, having;
	std::vector<ExprP> groupBy;
	std::vector<OrderItem> order;
	int64_t limit = -1, offset = 0;
	std::shared_ptr<SelectStmt> next;  // UNION chain
	bool unionAll = false;             // link between this and next
	int setOp = 0;                     // link between this and next: 0 UNION, 1 INTERSECT, 2 EXCEPT
	std::vector<OrderItem> uOrder;     // trailing ORDER BY / LIMIT of a UNION chain (applies to the whole result)
	int64_t uLimit = -1, uOffset = 0;
};
struct ColDef {
	std::string name, typeName;
	CType type = CType::Int;
	int len = 0;
	bool notNull = false, autoInc = false, hasDef = false, defNow = false, primary = false, unique = false;
	Value def;
	std::string check;  // column-level CHECK (...) condition text
	std::string gen;    // GENERATED ALWAYS AS (...) expression text
	bool genStored = false;
};
struct FkDef {
	std::string name, refDb, refTable;
	std::vector<std::string> cols, refCols;
	int onDelete = 0, onUpdate = 0;  // 0 NO ACTION, 1 CASCADE, 2 SET NULL, 3 RESTRICT, 4 SET DEFAULT (rejected)
};
struct Stmt {
	enum class K {
		Empty, Ignored, CreateDb, DropDb, Use, ShowDbs, ShowTables, ShowColumns, ShowCreate, ShowIndex, ShowTableStatus, ShowCreateDb, ShowCreateView, ShowVars, ShowWarnings, Explain, CreateView, DropView,
		CreateTable, DropTable, AlterTable, CreateIndex, DropIndex, Truncate, Insert, Select, Update, Delete, Begin, Commit, Rollback, SetVar
	};
	K k = K::Empty;
	std::string db, name;
	bool ifExists = false, ifNotExists = false;
	std::vector<ColDef> cols;
	std::vector<std::string> pkCols;
	std::vector<std::vector<std::string>> uniqueKeys;
	std::vector<std::string> uniqueNames;  // parallel to uniqueKeys; empty = default (first column)
	std::vector<std::pair<std::string, std::vector<std::string>>> keys;  // non-unique indexes (name, columns)
	std::vector<FkDef> fks;
	std::string like, after;  // SHOW ... LIKE pattern; ALTER ... AFTER column
	bool full = false, first = false;  // SHOW FULL ...; ALTER ... FIRST
	std::vector<std::pair<std::string, std::string>> checks;  // CHECK constraints (name, condition text)
	std::vector<std::string> insCols;
	std::vector<std::vector<ExprP>> rows;
	std::vector<std::pair<std::string, ExprP>> sets;
	ExprP where;
	std::vector<OrderItem> order;
	int64_t limit = -1;
	std::unique_ptr<SelectStmt> sel;
	int nParams = 0;  // number of ? placeholders (prepared statements)
	std::string text;  // CREATE VIEW: source text of the SELECT
	bool orReplace = false;
	bool ignore = false, replace = false;  // INSERT IGNORE / REPLACE INTO
	std::vector<std::pair<std::string, ExprP>> onDup;  // INSERT ... ON DUPLICATE KEY UPDATE
	std::string op, newName;  // ALTER / INDEX
	ColDef col;
	std::vector<std::string> idxCols;
	bool unique = false;
};