#include "parser.hpp"

#include <cerrno>
#include <set>

#include "lexer.hpp"

namespace {
	using SK = Stmt::K;
	using EK = Expr::K;

	bool ciEq(const std::string& a, const char* b) { return ciCompare(a, std::string(b)) == 0; }

	bool reserved(const std::string& w) {
		static const std::set<std::string> r = {
			"FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "JOIN", "INNER", "LEFT", "RIGHT", "CROSS",
			"OUTER", "ON", "UNION", "SET", "VALUES", "AS", "AND", "OR", "NOT", "USING", "NATURAL", "FULL",
			"OFFSET", "SELECT", "INTO", "DISTINCT", "WHEN", "THEN", "ELSE", "END", "IS", "IN", "LIKE",
			"BETWEEN", "DIV", "MOD" };
		return r.count(upper(w)) > 0;
	}

	class Parser {
	public:
		explicit Parser(const std::string& sql) : q_(sql), t_(lex(sql)) {}
		Stmt statement();

	private:
		const std::string& q_;
		std::vector<Token> t_;
		size_t p_ = 0;
		int params_ = 0;  // ? placeholders seen

		const Token& cur() const { return t_[p_]; }
		const Token& la(size_t k) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
		void adv() { if (p_ + 1 < t_.size()) ++p_; }
		[[noreturn]] void fail() const {
			throw SqlError(1064, "42000",
				"You have an error in your SQL syntax; check the manual that corresponds to your "
				"MySQL server version for the right syntax to use near '" +
				q_.substr(std::min(cur().pos, q_.size()), 30) + "' at line 1");
		}
		bool isKw(const char* k) const { return cur().type == Tok::Ident && ciEq(cur().text, k); }
		bool isKwAt(size_t o, const char* k) const { return la(o).type == Tok::Ident && ciEq(la(o).text, k); }
		bool isSym(const char* s) const { return cur().type == Tok::Sym && cur().text == s; }
		bool isSymAt(size_t o, const char* s) const { return la(o).type == Tok::Sym && la(o).text == s; }
		bool acceptKw(const char* k) { if (isKw(k)) { adv(); return true; } return false; }
		void expectKw(const char* k) { if (!acceptKw(k)) fail(); }
		bool acceptSym(const char* s) { if (isSym(s)) { adv(); return true; } return false; }
		void expectSym(const char* s) { if (!acceptSym(s)) fail(); }
		std::string ident() {
			if (cur().type == Tok::Ident || cur().type == Tok::QIdent) { std::string s = cur().text; adv(); return s; }
			fail();
		}
		void skipRest() { while (cur().type != Tok::End && !isSym(";")) adv(); }
		void tableName(std::string& db, std::string& name) {
			name = ident();
			if (acceptSym(".")) { db = name; name = ident(); }
		}
		bool aliasStart() const {
			return cur().type == Tok::QIdent || cur().type == Tok::Str || (cur().type == Tok::Ident && !reserved(cur().text));
		}
		std::string aliasName() {
			if (cur().type == Tok::Str) { std::string s = cur().text; adv(); return s; }
			return ident();
		}
		static ExprP mk(EK k) { auto e = std::make_unique<Expr>(); e->k = k; return e; }
		static ExprP bin(const std::string& op, ExprP a, ExprP b) {
			auto e = mk(EK::Bin); e->name = op; e->args.push_back(std::move(a)); e->args.push_back(std::move(b)); return e;
		}
		static ExprP lit(Value v) { auto e = mk(EK::Lit); e->v = std::move(v); return e; }

		ExprP expr() { return orExpr(); }
		ExprP orExpr() {
			auto l = andExpr();
			while (isKw("OR") || isSym("||")) { adv(); l = bin("OR", std::move(l), andExpr()); }
			return l;
		}
		ExprP andExpr() {
			auto l = notExpr();
			while (isKw("AND") || isSym("&&")) { adv(); l = bin("AND", std::move(l), notExpr()); }
			return l;
		}
		ExprP notExpr() {
			if (isKw("NOT") || isSym("!")) {
				adv();
				auto e = mk(EK::Unary); e->name = "NOT"; e->args.push_back(notExpr()); return e;
			}
			return cmpExpr();
		}
		ExprP cmpExpr() {
			auto l = addExpr();
			for (;;) {
				if (cur().type == Tok::Sym && (cur().text == "=" || cur().text == "<>" || cur().text == "!=" ||
					cur().text == "<" || cur().text == "<=" || cur().text == ">" ||
					cur().text == ">=" || cur().text == "<=>")) {
					std::string op = cur().text == "!=" ? "<>" : cur().text;
					adv();
					l = bin(op, std::move(l), addExpr());
					continue;
				}
				if (acceptKw("IS")) {
					auto e = mk(EK::IsNull);
					e->neg = acceptKw("NOT");
					expectKw("NULL");
					e->args.push_back(std::move(l));
					l = std::move(e);
					continue;
				}
				bool neg = false;
				if (isKw("NOT") && (isKwAt(1, "IN") || isKwAt(1, "BETWEEN") || isKwAt(1, "LIKE"))) { neg = true; adv(); }
				if (acceptKw("IN")) {
					auto e = mk(EK::In); e->neg = neg; e->args.push_back(std::move(l));
					expectSym("(");
					if (acceptKw("SELECT")) e->sub = selectBody();
					else do e->args.push_back(expr()); while (acceptSym(","));
					expectSym(")");
					l = std::move(e);
				}
				else if (acceptKw("BETWEEN")) {
					auto e = mk(EK::Between); e->neg = neg; e->args.push_back(std::move(l));
					e->args.push_back(addExpr());
					expectKw("AND");
					e->args.push_back(addExpr());
					l = std::move(e);
				}
				else if (acceptKw("LIKE")) {
					auto e = mk(EK::Like); e->neg = neg; e->args.push_back(std::move(l));
					e->args.push_back(addExpr());
					l = std::move(e);
				}
				else break;
			}
			return l;
		}
		ExprP addExpr() {
			auto l = mulExpr();
			while (isSym("+") || isSym("-")) { std::string op = cur().text; adv(); l = bin(op, std::move(l), mulExpr()); }
			return l;
		}
		ExprP mulExpr() {
			auto l = unary();
			for (;;) {
				if (isSym("*") || isSym("/") || isSym("%")) { std::string op = cur().text; adv(); l = bin(op, std::move(l), unary()); }
				else if (isKw("DIV")) { adv(); l = bin("DIV", std::move(l), unary()); }
				else if (isKw("MOD")) { adv(); l = bin("%", std::move(l), unary()); }
				else break;
			}
			return l;
		}
		ExprP unary() {
			if (acceptSym("-")) {
				auto o = unary();
				if (o->k == EK::Lit && o->v.t == Value::T::Int) { o->v.i = -o->v.i; return o; }
				if (o->k == EK::Lit && o->v.t == Value::T::Real) { o->v.d = -o->v.d; return o; }
				auto e = mk(EK::Unary); e->name = "-"; e->args.push_back(std::move(o)); return e;
			}
			if (acceptSym("+")) return unary();
			return primary();
		}
		static Value numLit(const Token& tk) {
			if (!tk.isReal) {
				errno = 0;
				long long v = std::strtoll(tk.text.c_str(), nullptr, 10);
				if (errno != ERANGE) return Value::integer(v);
			}
			if (tk.isReal && tk.text.find_first_of("eE") == std::string::npos) {  // exact decimal literal
				size_t dot = tk.text.find('.');
				int sc = dot == std::string::npos ? 0 : static_cast<int>(tk.text.size() - dot - 1);
				return Value::dec(std::strtod(tk.text.c_str(), nullptr), std::min(sc, 30));
			}
			return Value::real(std::strtod(tk.text.c_str(), nullptr));
		}
		ExprP primary() {
			const Token& tk = cur();
			if (tk.type == Tok::Num) { Value v = numLit(tk); adv(); return lit(v); }
			if (tk.type == Tok::Str) { Value v = Value::text(tk.text); adv(); return lit(v); }
			if (isSym("?")) {
				adv();
				auto e = mk(EK::Param);
				e->v = Value::integer(params_++);
				return e;
			}
			if (isKw("INTERVAL")) {  // INTERVAL <expr> <unit>, used by DATE_ADD / date +- INTERVAL
				adv();
				auto f = mk(EK::Func);
				f->name = "INTERVAL";
				f->args.push_back(addExpr());
				f->table = upper(ident());
				return f;
			}
			if (isSym("@")) {
				adv();
				bool sys = acceptSym("@");
				std::string nm = ident();
				if (sys && acceptSym(".")) nm = ident();  // @@session.x / @@global.x
				for (char& ch : nm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				auto f = mk(EK::Func);
				f->name = "@@VAR";
				f->table = (sys ? "" : "@") + nm;
				return f;
			}
			if (isSym("(") && isKwAt(1, "SELECT")) {
				adv(); adv();
				auto e = mk(EK::Sub); e->sub = selectBody(); expectSym(")"); return e;
			}
			if (isKw("EXISTS") && isSymAt(1, "(") && isKwAt(2, "SELECT")) {
				adv(); adv(); adv();
				auto e = mk(EK::Exists); e->sub = selectBody(); expectSym(")"); return e;
			}
			if (isSym("(")) { adv(); auto e = expr(); expectSym(")"); return e; }
			if (tk.type == Tok::Ident) {
				if (acceptKw("NULL")) return lit(Value());
				if (acceptKw("TRUE")) return lit(Value::integer(1));
				if (acceptKw("FALSE")) return lit(Value::integer(0));
				if (isKw("CASE")) return caseExpr();
				std::string up = upper(tk.text);
				if (isSymAt(1, "(")) return func();
				if (up == "CURRENT_TIMESTAMP" || up == "NOW" || up == "CURDATE" || up == "CURRENT_DATE" ||
					(up == "DEFAULT" && (isSymAt(1, ",") || isSymAt(1, ")")))) {
					adv();
					auto f = mk(EK::Func); f->name = up == "CURRENT_DATE" ? "CURDATE" : up; return f;
				}
			}
			if (tk.type == Tok::Ident || tk.type == Tok::QIdent) {
				auto e = mk(EK::Col);
				e->name = ident();
				if (acceptSym(".")) { e->table = e->name; e->name = ident(); }
				return e;
			}
			fail();
		}
		ExprP func() {
			auto f = mk(EK::Func);
			f->name = upper(cur().text);
			adv(); adv();
			if (acceptSym(")")) return f;
			if (isSym("*")) { adv(); f->star = true; expectSym(")"); return f; }
			if (acceptKw("DISTINCT")) f->distinct = true;
			do f->args.push_back(expr()); while (acceptSym(","));
			if (acceptKw("SEPARATOR")) { if (cur().type != Tok::Str) fail(); f->table = cur().text; adv(); }
			expectSym(")");
			return f;
		}
		ExprP caseExpr() {
			adv();
			auto e = mk(EK::Case);
			if (!isKw("WHEN")) { e->hasOperand = true; e->args.push_back(expr()); }
			if (!isKw("WHEN")) fail();
			while (acceptKw("WHEN")) { e->args.push_back(expr()); expectKw("THEN"); e->args.push_back(expr()); }
			if (acceptKw("ELSE")) { e->hasElse = true; e->args.push_back(expr()); }
			expectKw("END");
			return e;
		}

		TableRef tableRef() {
			TableRef r;
			tableName(r.db, r.name);
			if (acceptKw("AS")) r.alias = ident();
			else if (aliasStart()) r.alias = aliasName();
			return r;
		}
		void orderBy(std::vector<OrderItem>& out) {
			expectKw("BY");
			do {
				OrderItem o; o.e = expr();
				if (acceptKw("DESC")) o.desc = true; else acceptKw("ASC");
				out.push_back(std::move(o));
			} while (acceptSym(","));
		}
		int64_t intTok() {
			if (isSym("?")) { adv(); ++params_; return 0; }  // LIMIT ? (value supplied at execute time)
			if (cur().type != Tok::Num || cur().isReal) fail();
			int64_t v = std::strtoll(cur().text.c_str(), nullptr, 10);
			adv();
			return v;
		}
		std::unique_ptr<SelectStmt> selectCore() {
			auto s = std::make_unique<SelectStmt>();
			if (acceptKw("DISTINCT")) s->distinct = true; else acceptKw("ALL");
			do {
				SelItem it;
				if (isSym("*")) { adv(); it.star = true; }
				else if ((cur().type == Tok::Ident || cur().type == Tok::QIdent) && isSymAt(1, ".") && isSymAt(2, "*")) {
					it.starTable = ident(); adv(); adv(); it.star = true;
				}
				else {
					size_t st = cur().pos;
					it.e = expr();
					it.text = q_.substr(st, t_[p_ - 1].end - st);
					if (acceptKw("AS")) it.alias = aliasName();
					else if (aliasStart()) it.alias = aliasName();
				}
				s->items.push_back(std::move(it));
			} while (acceptSym(","));
			if (acceptKw("FROM")) {
				s->hasFrom = true;
				s->from = tableRef();
				for (;;) {
					JoinClause j;
					if (acceptSym(",")) { j.type = JoinClause::Type::Cross; j.ref = tableRef(); }
					else if (isKw("JOIN") || isKw("INNER") || isKw("CROSS") || isKw("LEFT") || isKw("RIGHT")) {
						if (acceptKw("INNER")) j.type = JoinClause::Type::Inner;
						else if (acceptKw("CROSS")) j.type = JoinClause::Type::Cross;
						else if (acceptKw("LEFT")) { acceptKw("OUTER"); j.type = JoinClause::Type::Left; }
						else if (acceptKw("RIGHT")) { acceptKw("OUTER"); j.type = JoinClause::Type::Right; }
						expectKw("JOIN");
						j.ref = tableRef();
						if (acceptKw("ON")) j.on = expr();
						else if (j.type == JoinClause::Type::Left || j.type == JoinClause::Type::Right) fail();
					}
					else break;
					s->joins.push_back(std::move(j));
				}
			}
			if (acceptKw("WHERE")) s->where = expr();
			if (acceptKw("GROUP")) { expectKw("BY"); do s->groupBy.push_back(expr()); while (acceptSym(",")); }
			if (acceptKw("HAVING")) s->having = expr();
			if (acceptKw("ORDER")) orderBy(s->order);
			if (acceptKw("LIMIT")) {
				int64_t a = intTok();
				if (acceptSym(",")) { s->offset = a; s->limit = intTok(); }
				else { s->limit = a; if (acceptKw("OFFSET")) s->offset = intTok(); }
			}
			return s;
		}
		std::unique_ptr<SelectStmt> selectBody() {
			auto s = selectCore();
			SelectStmt* last = s.get();
			while (acceptKw("UNION")) {
				last->unionAll = acceptKw("ALL");
				if (!last->unionAll) acceptKw("DISTINCT");
				expectKw("SELECT");
				last->next = selectCore();
				last = last->next.get();
			}
			if (last != s.get()) {  // a trailing ORDER BY / LIMIT applies to the whole UNION
				s->uOrder = std::move(last->order);
				s->uLimit = last->limit;
				s->uOffset = last->offset;
				last->order.clear();
				last->limit = -1;
				last->offset = 0;
			}
			return s;
		}

		std::vector<std::string> identList() {
			std::vector<std::string> v;
			expectSym("(");
			do {
				v.push_back(ident());
				if (acceptSym("(")) { intTok(); expectSym(")"); }
				if (!acceptKw("ASC")) acceptKw("DESC");
			} while (acceptSym(","));
			expectSym(")");
			return v;
		}
		void skipItem() {
			int depth = 0;
			while (cur().type != Tok::End) {
				if (isSym("(")) ++depth;
				else if (isSym(")")) { if (depth == 0) break; --depth; }
				else if (isSym(",") && depth == 0) break;
				adv();
			}
		}
		void colType(ColDef& c) {
			std::string base = upper(ident());
			if (base == "DOUBLE") acceptKw("PRECISION");
			// Aliases (incl. Oracle-style character types)
			if (base == "INTEGER") base = "INT";
			if (base == "VARCHAR2" || base == "NVARCHAR2" || base == "NVARCHAR") base = "VARCHAR";
			if (base == "NCHAR") base = "CHAR";
			static const std::set<std::string> ints = { "INT", "INTEGER", "SMALLINT", "TINYINT", "MEDIUMINT", "BIGINT", "BOOL", "BOOLEAN" };
			static const std::set<std::string> reals = { "FLOAT", "DOUBLE", "REAL", "DECIMAL", "NUMERIC", "DEC" };
			static const std::set<std::string> texts = { "CHAR", "VARCHAR", "TEXT", "TINYTEXT", "MEDIUMTEXT", "LONGTEXT",
														"BLOB", "TINYBLOB", "MEDIUMBLOB", "LONGBLOB", "TIME", "YEAR", "ENUM" };
			if (ints.count(base)) c.type = CType::Int;
			else if (reals.count(base)) c.type = CType::Real;
			else if (texts.count(base)) c.type = CType::Text;
			else if (base == "DATE") c.type = CType::Date;
			else if (base == "DATETIME" || base == "TIMESTAMP") c.type = CType::DateTime;
			else fail();
			std::string lower = base;
			for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			c.typeName = lower;
			if (base == "BOOL" || base == "BOOLEAN") c.typeName = "tinyint(1)";
			if (acceptSym("(")) {
				if (base == "ENUM") {
					while (cur().type != Tok::End && !isSym(")")) adv();
				}
				else {
					int64_t a = intTok(), b = -1;
					if (acceptSym(",")) b = intTok();
					if (!acceptKw("BYTE")) acceptKw("CHAR");  // Oracle VARCHAR2(n BYTE|CHAR)
					c.len = static_cast<int>(a);
					if (base == "CHAR" || base == "VARCHAR") c.typeName += "(" + std::to_string(a) + ")";
					else if (base == "DECIMAL" || base == "NUMERIC" || base == "DEC")
						c.typeName += "(" + std::to_string(a) + "," + std::to_string(b < 0 ? 0 : b) + ")";
				}
				expectSym(")");
			}
			if (base != "CHAR" && base != "VARCHAR") c.len = 0;
			if (base == "ENUM") c.typeName = "enum";
		}
		void colDefault(ColDef& c) {
			if (acceptKw("NULL")) { c.hasDef = true; return; }
			if (isKw("CURRENT_TIMESTAMP") || isKw("NOW")) {
				adv();
				if (acceptSym("(")) expectSym(")");
				c.hasDef = c.defNow = true;
				return;
			}
			auto e = unary();
			if (e->k != EK::Lit) fail();
			c.def = e->v;
			c.hasDef = true;
		}
		ColDef columnDef() {
			ColDef c;
			c.name = ident();
			colType(c);
			for (;;) {
				if (acceptKw("NOT")) { expectKw("NULL"); c.notNull = true; }
				else if (acceptKw("NULL")) c.notNull = false;
				else if (acceptKw("DEFAULT")) colDefault(c);
				else if (acceptKw("AUTO_INCREMENT")) c.autoInc = true;
				else if (acceptKw("PRIMARY")) { expectKw("KEY"); c.primary = true; }
				else if (acceptKw("KEY")) c.primary = true;
				else if (acceptKw("UNIQUE")) { acceptKw("KEY"); c.unique = true; }
				else if (acceptKw("COMMENT")) { if (cur().type != Tok::Str) fail(); adv(); }
				else if (acceptKw("COLLATE")) ident();
				else if (acceptKw("CHARACTER")) { expectKw("SET"); ident(); }
				else if (acceptKw("CHARSET")) ident();
				else if (acceptKw("ON")) { expectKw("UPDATE"); ident(); if (acceptSym("(")) expectSym(")"); }
				else if (acceptKw("UNSIGNED")) c.typeName += " unsigned";
				else if (acceptKw("ZEROFILL") || acceptKw("SIGNED")) {}
				else break;
			}
			return c;
		}
		void tableItem(Stmt& s) {
			if (isKw("PRIMARY")) { adv(); expectKw("KEY"); s.pkCols = identList(); skipItem(); }
			else if (isKw("UNIQUE")) {
				adv();
				if (!acceptKw("KEY")) acceptKw("INDEX");
				if (!isSym("(")) ident();
				s.uniqueKeys.push_back(identList());
				skipItem();
			}
			else if (isKw("KEY") || isKw("INDEX")) {
				adv();
				std::string nm;
				if (!isSym("(")) nm = ident();
				auto cols = identList();
				if (nm.empty()) nm = cols[0];
				s.keys.emplace_back(nm, cols);
				skipItem();
			}
			else if (isKw("FOREIGN") || isKw("FULLTEXT") || isKw("CHECK")) skipItem();
			else if (isKw("CONSTRAINT")) {
				adv();
				if (!isKw("PRIMARY") && !isKw("UNIQUE") && !isKw("FOREIGN") && !isKw("CHECK")) ident();
				tableItem(s);
			}
			else s.cols.push_back(columnDef());
		}
		void createTable(Stmt& s) {
			s.k = SK::CreateTable;
			if (acceptKw("IF")) { expectKw("NOT"); expectKw("EXISTS"); s.ifNotExists = true; }
			tableName(s.db, s.name);
			expectSym("(");
			do tableItem(s); while (acceptSym(","));
			expectSym(")");
			skipRest();
		}
		void insert(Stmt& s) {
			s.k = SK::Insert;
			acceptKw("INTO");
			tableName(s.db, s.name);
			if (isSym("(")) s.insCols = identList();
			if (acceptKw("VALUES") || acceptKw("VALUE")) {
				do {
					expectSym("(");
					std::vector<ExprP> row;
					if (!isSym(")")) do row.push_back(expr()); while (acceptSym(","));
					expectSym(")");
					s.rows.push_back(std::move(row));
				} while (acceptSym(","));
			}
			else if (acceptKw("SELECT")) s.sel = selectBody();
			else fail();
		}
		void update(Stmt& s) {
			s.k = SK::Update;
			tableName(s.db, s.name);
			expectKw("SET");
			do {
				std::string c = ident();
				if (acceptSym(".")) c = ident();
				expectSym("=");
				s.sets.emplace_back(c, expr());
			} while (acceptSym(","));
			tail(s);
		}
		void tail(Stmt& s) {
			if (acceptKw("WHERE")) s.where = expr();
			if (acceptKw("ORDER")) orderBy(s.order);
			if (acceptKw("LIMIT")) s.limit = intTok();
		}
		void alter(Stmt& s) {
			expectKw("TABLE");
			s.k = SK::AlterTable;
			tableName(s.db, s.name);
			if (acceptKw("ADD")) {
				if (isKw("UNIQUE") || isKw("INDEX") || isKw("KEY")) {
					bool uq = acceptKw("UNIQUE");
					if (!acceptKw("KEY")) acceptKw("INDEX");
					if (!isSym("(")) s.newName = ident();
					s.idxCols = identList();
					if (s.newName.empty()) s.newName = s.idxCols[0];
					s.op = uq ? "UNIQUE" : "INDEX";
				}
				else if (isKw("PRIMARY")) {
					adv();
					expectKw("KEY");
					s.idxCols = identList();
					s.op = "PK";
				}
				else { acceptKw("COLUMN"); s.op = "ADD"; s.col = columnDef(); }
			}
			else if (acceptKw("DROP")) {
				if (isKw("PRIMARY")) { adv(); expectKw("KEY"); s.op = "DROPPK"; }
				else if (acceptKw("INDEX") || acceptKw("KEY")) { s.op = "DROPINDEX"; s.newName = ident(); }
				else { acceptKw("COLUMN"); s.op = "DROP"; s.newName = ident(); }
			}
			else if (acceptKw("MODIFY")) { acceptKw("COLUMN"); s.op = "MODIFY"; s.col = columnDef(); }
			else if (acceptKw("CHANGE")) { acceptKw("COLUMN"); s.op = "CHANGE"; s.newName = ident(); s.col = columnDef(); }
			else if (acceptKw("RENAME")) {
				if (acceptKw("COLUMN")) { s.op = "RENAMECOL"; s.newName = ident(); expectKw("TO"); s.col.name = ident(); }
				else { if (!acceptKw("TO")) acceptKw("AS"); s.op = "RENAME"; s.newName = ident(); }
			}
			else fail();
		}
		void show(Stmt& s) {
			if (acceptKw("DATABASES") || acceptKw("SCHEMAS")) s.k = SK::ShowDbs;
			else if (acceptKw("TABLES")) { s.k = SK::ShowTables; if (acceptKw("FROM") || acceptKw("IN")) s.db = ident(); }
			else if (acceptKw("CREATE")) { expectKw("TABLE"); s.k = SK::ShowCreate; tableName(s.db, s.name); }
			else if (acceptKw("WARNINGS")) s.k = SK::ShowWarnings;
			else if (isKw("GLOBAL") || isKw("SESSION") || isKw("VARIABLES") || isKw("STATUS")) {
				if (!acceptKw("GLOBAL")) acceptKw("SESSION");
				bool st = acceptKw("STATUS");
				if (!st) expectKw("VARIABLES");
				s.k = SK::ShowVars;
				s.op = st ? "status" : "vars";
				if (acceptKw("LIKE")) { if (cur().type != Tok::Str) fail(); s.newName = cur().text; adv(); }
			}
			else {
				acceptKw("FULL");
				if (!(acceptKw("COLUMNS") || acceptKw("FIELDS"))) fail();
				if (!(acceptKw("FROM") || acceptKw("IN"))) fail();
				s.k = SK::ShowColumns;
				tableName(s.db, s.name);
				if (acceptKw("FROM") || acceptKw("IN")) s.db = ident();
			}
		}
	};

	Stmt Parser::statement() {
		Stmt s;
		if (cur().type == Tok::End) return s;
		if (acceptKw("SELECT")) { s.k = SK::Select; s.sel = selectBody(); }
		else if (acceptKw("INSERT")) insert(s);
		else if (acceptKw("UPDATE")) update(s);
		else if (acceptKw("DELETE")) { s.k = SK::Delete; expectKw("FROM"); tableName(s.db, s.name); tail(s); }
		else if (acceptKw("CREATE")) {
			if (acceptKw("DATABASE") || acceptKw("SCHEMA")) {
				s.k = SK::CreateDb;
				if (acceptKw("IF")) { expectKw("NOT"); expectKw("EXISTS"); s.ifNotExists = true; }
				s.name = ident();
				skipRest();
			}
			else if (acceptKw("TABLE")) createTable(s);
			else if (isKw("OR") || isKw("VIEW")) {
				bool repl = false;
				if (acceptKw("OR")) { expectKw("REPLACE"); repl = true; }
				expectKw("VIEW");
				s.k = SK::CreateView;
				s.orReplace = repl;
				tableName(s.db, s.name);
				if (isSym("(")) s.idxCols = identList();
				expectKw("AS");
				if (!isKw("SELECT")) fail();
				size_t st = cur().pos;
				adv();
				s.sel = selectBody();
				s.text = q_.substr(st, t_[p_ - 1].end - st);
			}
			else if (isKw("UNIQUE") || isKw("INDEX")) {
				bool uq = acceptKw("UNIQUE");
				expectKw("INDEX");
				s.k = SK::CreateIndex; s.unique = uq; s.newName = ident();
				expectKw("ON");
				tableName(s.db, s.name);
				s.idxCols = identList();
				skipRest();
			}
			else fail();
		}
		else if (acceptKw("DROP")) {
			if (acceptKw("VIEW")) {
				s.k = SK::DropView;
				if (acceptKw("IF")) { expectKw("EXISTS"); s.ifExists = true; }
				tableName(s.db, s.name);
			}
			else if (acceptKw("INDEX")) {
				s.k = SK::DropIndex; s.newName = ident();
				expectKw("ON");
				tableName(s.db, s.name);
			}
			else {
				bool db = acceptKw("DATABASE") || acceptKw("SCHEMA");
				if (!db) expectKw("TABLE");
				s.k = db ? SK::DropDb : SK::DropTable;
				if (acceptKw("IF")) { expectKw("EXISTS"); s.ifExists = true; }
				if (db) s.name = ident(); else tableName(s.db, s.name);
			}
		}
		else if (acceptKw("ALTER")) alter(s);
		else if (acceptKw("USE")) { s.k = SK::Use; s.name = ident(); }
		else if (acceptKw("SHOW")) show(s);
		else if (isKw("DESCRIBE") || isKw("DESC") || isKw("EXPLAIN")) {
			bool ex = isKw("EXPLAIN");
			adv();
			if (ex && acceptKw("SELECT")) { s.k = SK::Explain; s.sel = selectBody(); }
			else { s.k = SK::ShowColumns; tableName(s.db, s.name); }
		}
		else if (acceptKw("TRUNCATE")) { acceptKw("TABLE"); s.k = SK::Truncate; tableName(s.db, s.name); }
		else if (acceptKw("BEGIN")) s.k = SK::Begin;
		else if (acceptKw("START")) { expectKw("TRANSACTION"); s.k = SK::Begin; }
		else if (acceptKw("COMMIT")) s.k = SK::Commit;
		else if (acceptKw("ROLLBACK")) s.k = SK::Rollback;
		else if (isKw("SET")) { s.k = SK::Ignored; skipRest(); }
		else fail();
		acceptSym(";");
		if (cur().type != Tok::End) fail();
		s.nParams = params_;
		return s;
	}
}  // namespace

Stmt parseStatement(const std::string& sql) { return Parser(sql).statement(); }