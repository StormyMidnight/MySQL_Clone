#pragma once
#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

struct Expr;
struct SelectStmt;
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
	std::shared_ptr<SelectStmt> sub;  // Sub / Exists / IN (SELECT ...)
	mutable std::shared_ptr<std::vector<std::vector<Value>>> cache;  // uncorrelated subquery result
};

struct SelItem { ExprP e; std::string alias, text, starTable; bool star = false; };
struct TableRef { std::string db, name, alias; };
struct JoinClause {
	enum class Type { Inner, Left, Right, Cross } type = Type::Inner;
	TableRef ref;
	ExprP on;
};
struct OrderItem { ExprP e; bool desc = false; };
struct SelectStmt {
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
	std::vector<OrderItem> uOrder;     // trailing ORDER BY / LIMIT of a UNION chain (applies to the whole result)
	int64_t uLimit = -1, uOffset = 0;
};
struct ColDef {
	std::string name, typeName;
	CType type = CType::Int;
	int len = 0;
	bool notNull = false, autoInc = false, hasDef = false, defNow = false, primary = false, unique = false;
	Value def;
};
struct Stmt {
	enum class K {
		Empty, Ignored, CreateDb, DropDb, Use, ShowDbs, ShowTables, ShowColumns, ShowCreate, ShowVars, ShowWarnings, Explain, CreateView, DropView,
		CreateTable, DropTable, AlterTable, CreateIndex, DropIndex, Truncate, Insert, Select, Update, Delete, Begin, Commit, Rollback
	};
	K k = K::Empty;
	std::string db, name;
	bool ifExists = false, ifNotExists = false;
	std::vector<ColDef> cols;
	std::vector<std::string> pkCols;
	std::vector<std::vector<std::string>> uniqueKeys;
	std::vector<std::pair<std::string, std::vector<std::string>>> keys;  // non-unique indexes (name, columns)
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
	std::string op, newName;  // ALTER / INDEX
	ColDef col;
	std::vector<std::string> idxCols;
	bool unique = false;
};