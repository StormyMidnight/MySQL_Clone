#include "executor.hpp"

#include <regex>
#include "json.hpp"
#include <tuple>
#include <cstdio>
#include <ctime>
#include <limits>
#include <unistd.h>
#include <set>
#include <unordered_map>

#include "parser.hpp"

namespace {
    using EK = Expr::K;
    using SK = Stmt::K;

    struct ColRef { std::string table, name; CType type = CType::Int; };
    struct Scope { std::vector<ColRef> cols; };
    struct Ctx {
        const Scope* scope;
        const Row* row;
        const std::vector<const Row*>* group;
        const Env* env;
        const std::vector<SelItem>* items;  // for alias lookup (HAVING / ORDER BY)
        const Ctx* outer = nullptr;         // enclosing query's context (correlated subqueries)
    };

    Value B(bool b) { return Value::integer(b ? 1 : 0); }

    // Scale of an exact numeric (INT = 0, DECIMAL = its scale); -1 for floating point / text.
    int exactScale(const Value& v) {
        if (v.t == Value::T::Int) return 0;
        return v.t == Value::T::Real ? v.scale : -1;
    }

    std::string keyOf(const Value& v) {
        switch (v.t) {
        case Value::T::Null: return "N";
        case Value::T::Int:
        case Value::T::Real: { char b[48]; std::snprintf(b, sizeof b, "#%.17g", v.asReal()); return b; }
        case Value::T::Text: { std::string s = "T"; for (char c : v.s) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
        }
        return {};
    }

    size_t resolveCol(const Expr& e, const Scope& sc) {
        if (e.slot >= 0) return static_cast<size_t>(e.slot);
        int found = -1;
        for (size_t i = 0; i < sc.cols.size(); ++i) {
            if (ciCompare(sc.cols[i].name, e.name) != 0) continue;
            if (!e.table.empty() && sc.cols[i].table != e.table) continue;
            if (found >= 0) throw SqlError(1052, "23000", "Column '" + e.name + "' in field list is ambiguous");
            found = static_cast<int>(i);
        }
        if (found < 0)
            throw SqlError(1054, "42S22", "Unknown column '" + (e.table.empty() ? "" : e.table + ".") + e.name + "' in 'field list'");
        e.slot = found;
        return static_cast<size_t>(found);
    }

    bool isAgg(const std::string& n) { return n == "COUNT" || n == "SUM" || n == "AVG" || n == "MIN" || n == "MAX" || n == "GROUP_CONCAT" || n == "JSON_ARRAYAGG" || n == "JSON_OBJECTAGG"; }

    // SQL value -> JSON value: JSON-flagged text is a document, other text a string
    jsn::J jvalOf(const Value& v) {
        if (v.isNull()) return jsn::J();
        if (v.t == Value::T::Int) return jsn::J::integer(v.i);
        if (v.t == Value::T::Real) return jsn::J::real(v.d);
        if (v.json) { jsn::J j; if (jsn::parse(v.s, j)) return j; }
        return jsn::J::str(v.s);
    }
    Value jvalue(const jsn::J& j) { Value x = Value::text(jsn::dump(j)); x.json = true; return x; }

    bool hasAgg(const Expr& e) {
        if (e.k == EK::Func && isAgg(e.name) && !e.win) return true;
        for (const auto& a : e.args) if (hasAgg(*a)) return true;
        if (e.win) {
            for (const auto& p : e.win->part) if (hasAgg(*p)) return true;
            for (const auto& o : e.win->order) if (hasAgg(*o.e)) return true;
        }
        return false;
    }
    void collectWin(const Expr& e, std::vector<const Expr*>& out) {
        if (e.k == EK::Func && e.win) { out.push_back(&e); return; }
        for (const auto& a : e.args) collectWin(*a, out);
    }

    std::string bq(const std::string& s) { return "`" + s + "`"; }
    std::string lowerStr(std::string s) { for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; }
    bool likeMatch(const std::string& s, size_t si, const std::string& p, size_t pi, char esc = '\\') {
        while (pi < p.size()) {
            char c = p[pi];
            if (c == '%') {
                while (pi < p.size() && p[pi] == '%') ++pi;
                if (pi == p.size()) return true;
                for (size_t k = si; k <= s.size(); ++k) if (likeMatch(s, k, p, pi, esc)) return true;
                return false;
            }
            if (si >= s.size()) return false;
            if (c == esc && esc != '\0' && pi + 1 < p.size()) { ++pi; c = p[pi]; if (std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(s[si]))) return false; }
            else if (c != '_' && std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(s[si]))) return false;
            ++si; ++pi;
        }
        return si == s.size();
    }

    std::string nowStr(bool dateOnly, std::time_t t) {
        if (t == 0) t = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&t, &tmv);
        char b[32];
        std::strftime(b, sizeof b, dateOnly ? "%Y-%m-%d" : "%Y-%m-%d %H:%M:%S", &tmv);
        return b;
    }

    Value eval(const Expr& e, const Ctx& c);

    std::shared_ptr<const std::vector<Row>> subRows(const Expr& e, const Ctx& c) {
        if (e.cache) return e.cache;
        int before = c.env->outerRefs;
        auto rows = std::make_shared<std::vector<Row>>(c.env->sub(*e.sub, &c));
        if (e.correlated || c.env->outerRefs != before) { e.correlated = true; return rows; }  // depends on the outer row
        e.cache = rows;
        return rows;
    }

    Value evalAgg(const Expr& e, const Ctx& c) {
        if (!c.group) throw SqlError(1111, "HY000", "Invalid use of group function");
        if (e.star) return Value::integer(static_cast<int64_t>(c.group->size()));
        if (e.args.empty()) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + e.name + "'");
        if (e.name == "JSON_ARRAYAGG" || e.name == "JSON_OBJECTAGG") {
            const bool isObj = e.name == "JSON_OBJECTAGG";
            if (e.args.size() != (isObj ? 2u : 1u)) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + e.name + "'");
            if (c.group->empty()) return Value();
            jsn::J out = isObj ? jsn::J::obj() : jsn::J::arr();
            Ctx sub2{ c.scope, nullptr, nullptr, c.env, nullptr, c.outer };
            for (const Row* r : *c.group) {
                sub2.row = r;
                if (isObj) {
                    Value k = eval(*e.args[0], sub2);
                    if (k.isNull()) throw SqlError(3158, "22032", "JSON documents may not contain NULL member names.");
                    jsn::objSet(out, k.str(), jvalOf(eval(*e.args[1], sub2)));
                }
                else out.a.push_back(jvalOf(eval(*e.args[0], sub2)));
            }
            return jvalue(out);
        }
        std::vector<Value> vals;
        std::set<std::string> seen;
        Ctx sub{ c.scope, nullptr, nullptr, c.env, nullptr, c.outer };
        for (const Row* r : *c.group) {
            sub.row = r;
            Value v = eval(*e.args[0], sub);
            if (v.isNull()) continue;
            if (e.distinct && !seen.insert(keyOf(v)).second) continue;
            vals.push_back(std::move(v));
        }
        if (e.name == "COUNT") return Value::integer(static_cast<int64_t>(vals.size()));
        if (vals.empty()) return Value();
        if (e.name == "SUM" || e.name == "AVG") {
            bool allInt = true, exact = true;
            double sd = 0;
            int64_t si = 0;
            int sc = 0;
            for (const Value& v : vals) {
                if (v.t != Value::T::Int) allInt = false;
                int es = exactScale(v);
                if (es < 0) exact = false; else sc = std::max(sc, es);
                sd += v.asReal();
                si += v.t == Value::T::Int ? v.i : 0;
            }
            double n = static_cast<double>(vals.size());
            if (e.name == "AVG") return exact ? Value::dec(sd / n, std::min(sc + 4, 30)) : Value::real(sd / n);
            if (allInt) return Value::integer(si);
            return exact ? Value::dec(sd, sc) : Value::real(sd);
        }
        if (e.name == "MIN" || e.name == "MAX") {
            Value best = vals[0];
            for (const Value& v : vals) {
                int cmp = compareValues(v, best);
                if ((e.name == "MIN" && cmp < 0) || (e.name == "MAX" && cmp > 0)) best = v;
            }
            return best;
        }
        std::string sep = e.table.empty() ? "," : e.table, out;
        for (size_t i = 0; i < vals.size(); ++i) { if (i) out += sep; out += vals[i].str(); }
        return Value::text(out);
    }

    // ---- date/time helpers (DATE/DATETIME are stored as normalized ISO text) ----
    bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    int daysInMonth(int y, int m) {
        static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        return m == 2 && leapYear(y) ? 29 : d[m - 1];
    }
    struct DT { int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0; };

    // Accepts 'YYYY-M-D' ('/' ok) optionally followed by ' ' or 'T' and h:mm[:ss[.frac]]. Validates ranges.
    bool parseDT(std::string s, DT& o, bool& hasTime) {
        size_t b = s.find_first_not_of(' ');
        if (b == std::string::npos) return false;
        s = s.substr(b, s.find_last_not_of(' ') - b + 1);
        for (size_t i = 0; i < s.size() && s[i] != ' ' && s[i] != 'T'; ++i) if (s[i] == '/') s[i] = '-';
        int n = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%d%n", &o.y, &o.mo, &o.d, &n) != 3) return false;
        o.h = o.mi = o.s = 0;
        hasTime = false;
        std::string rest = s.substr(static_cast<size_t>(n));
        if (!rest.empty()) {
            if (rest[0] != ' ' && rest[0] != 'T') return false;
            const char* p = rest.c_str() + 1;
            int k = 0;
            if (std::sscanf(p, "%d:%d%n", &o.h, &o.mi, &k) != 2) return false;
            p += k;
            if (*p == ':') { ++p; if (std::sscanf(p, "%d%n", &o.s, &k) != 1) return false; p += k; }
            if (*p == '.') { ++p; while (std::isdigit(static_cast<unsigned char>(*p))) ++p; }
            if (*p != '\0') return false;
            hasTime = true;
        }
        if (o.y < 0 || o.y > 9999 || o.mo < 1 || o.mo > 12 || o.d < 1 || o.d > daysInMonth(o.y, o.mo)) return false;
        return o.h >= 0 && o.h <= 23 && o.mi >= 0 && o.mi <= 59 && o.s >= 0 && o.s <= 59;
    }

    std::string fmtDT(const DT& t, bool withTime) {
        char b[32];
        if (withTime) std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.y, t.mo, t.d, t.h, t.mi, t.s);
        else std::snprintf(b, sizeof b, "%04d-%02d-%02d", t.y, t.mo, t.d);
        return b;
    }

    // DATE/DATETIME column vs loosely formatted string literal: normalize the literal (MySQL converts it).
    void alignDates(CType t, Value& colV, Value& other) {
        if ((t != CType::Date && t != CType::DateTime) || other.t != Value::T::Text || colV.t != Value::T::Text) return;
        DT d;
        bool ht = false;
        if (!parseDT(other.s, d, ht)) return;
        if (t == CType::Date && !ht) { other = Value::text(fmtDT(d, false)); return; }
        other = Value::text(fmtDT(d, true));
        if (t == CType::Date) colV = Value::text(colV.s + " 00:00:00");
    }
    CType colType(const Expr& e, const Ctx& c) {
        if (!c.scope || e.k != EK::Col || e.slot < 0) return CType::Int;
        return c.scope->cols[static_cast<size_t>(e.slot)].type;
    }

    int64_t daysFromCivil(int64_t y, int m, int d) {  // days since 1970-01-01 (proleptic Gregorian)
        y -= m <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
        int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    }
    void civilFromDays(int64_t z, int& y, int& m, int& d) {
        z += 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        y = static_cast<int>(yoe + era * 400 + (m <= 2));
    }

    Value addInterval(const Value& dv, int64_t amt, const std::string& unit, int sign) {
        DT t;
        bool hasTime = false;
        if (dv.isNull() || !parseDT(dv.str(), t, hasTime)) return Value();
        amt *= sign;
        bool timeUnit = unit == "HOUR" || unit == "MINUTE" || unit == "SECOND";
        if (unit == "MONTH" || unit == "QUARTER" || unit == "YEAR") {
            int64_t k = unit == "MONTH" ? amt : (unit == "QUARTER" ? amt * 3 : amt * 12);
            int64_t tot = static_cast<int64_t>(t.y) * 12 + (t.mo - 1) + k;
            if (tot < 0 || tot / 12 > 9999) return Value();
            t.y = static_cast<int>(tot / 12);
            t.mo = static_cast<int>(tot % 12) + 1;
            t.d = std::min(t.d, daysInMonth(t.y, t.mo));  // Jan 31 + 1 month = Feb 28/29
        }
        else if (unit == "DAY" || unit == "WEEK" || timeUnit) {
            int64_t unitSecs = unit == "DAY" ? 86400 : unit == "WEEK" ? 604800 : unit == "HOUR" ? 3600 : unit == "MINUTE" ? 60 : 1;
            int64_t total = daysFromCivil(t.y, t.mo, t.d) * 86400 + static_cast<int64_t>(t.h) * 3600 + t.mi * 60 + t.s + unitSecs * amt;
            int64_t days = total >= 0 ? total / 86400 : -((-total + 86399) / 86400), rem = total - days * 86400;
            civilFromDays(days, t.y, t.mo, t.d);
            if (t.y < 0 || t.y > 9999) return Value();
            t.h = static_cast<int>(rem / 3600);
            t.mi = static_cast<int>(rem % 3600 / 60);
            t.s = static_cast<int>(rem % 60);
        }
        else return Value();
        return Value::text(fmtDT(t, hasTime || timeUnit));
    }

    void intRange(const std::string& typeName, int64_t& lo, int64_t& hi) {
        bool u = typeName.find("unsigned") != std::string::npos;
        std::string base = typeName.substr(0, typeName.find_first_of("( "));
        int bits = base == "tinyint" ? 8 : base == "smallint" ? 16 : base == "mediumint" ? 24 : (base == "int" || base == "integer") ? 32 : 64;
        if (bits == 64) { lo = u ? 0 : std::numeric_limits<int64_t>::min(); hi = std::numeric_limits<int64_t>::max(); return; }
        lo = u ? 0 : -(int64_t(1) << (bits - 1));
        hi = u ? (int64_t(1) << bits) - 1 : (int64_t(1) << (bits - 1)) - 1;
    }

    // DECIMAL(p,s) / NUMERIC / DEC from the stored type name ("decimal", "decimal(10,2)"). Bare DECIMAL = (10,0).
    bool decParams(const std::string& tn, int& p, int& sc) {
        std::string base = tn.substr(0, tn.find_first_of("( "));
        if (base != "decimal" && base != "numeric" && base != "dec") return false;
        p = 10;
        sc = 0;
        size_t lp = tn.find('(');
        if (lp != std::string::npos) {
            int a = 0, b = 0;
            int n = std::sscanf(tn.c_str() + lp, "(%d,%d", &a, &b);
            if (n >= 1) p = a;
            if (n == 2) sc = b;
        }
        return true;
    }

    bool textual(CType t) { return t != CType::Int && t != CType::Real; }

    const std::map<std::string, std::string>& sysVars() {
        static const std::map<std::string, std::string> m = {
            { "auto_increment_increment", "1" },{ "autocommit", "1" },{ "character_set_client", "utf8mb4" },
            { "character_set_connection", "utf8mb4" },{ "character_set_results", "utf8mb4" },
            { "character_set_server", "utf8mb4" },{ "collation_connection", "utf8mb4_general_ci" },
            { "collation_server", "utf8mb4_general_ci" },{ "interactive_timeout", "28800" },{ "license", "GPL" },
            { "lower_case_table_names", "0" },{ "max_allowed_packet", "67108864" },{ "net_write_timeout", "60" },
            { "sql_mode", "" },{ "system_time_zone", "UTC" },{ "time_zone", "SYSTEM" },
            { "transaction_isolation", "REPEATABLE-READ" },{ "tx_isolation", "REPEATABLE-READ" },
            { "version", "8.0.0-mysqlclone" },{ "version_comment", "mysqlclone" },{ "wait_timeout", "28800" } };
        return m;
    }

    Value sysVar(const std::string& name) {
        auto it = sysVars().find(name);
        if (it == sysVars().end()) return Value();
        const std::string& v = it->second;
        bool num = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
        return num ? Value::integer(std::atoll(v.c_str())) : Value::text(v);
    }

    size_t u8len(const std::string& s) { size_t n = 0; for (char ch : s) if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n; return n; }

    Value scalarFunc(const Expr& e, const Ctx& c) {
        const std::string& n = e.name;
        if (n == "@@VAR") {
            if (!e.table.empty() && e.table[0] == '@') {
                auto it = c.env->vars->find(e.table);
                return it == c.env->vars->end() ? Value() : it->second;
            }
            return sysVar(e.table);
        }
        if (n == "@@ASSIGN") {
            Value v = eval(*e.args[0], c);
            (*c.env->vars)[e.table] = v;
            return v;
        }
        if (n == "VALUES") {
            if (e.args.size() != 1 || e.args[0]->k != EK::Col)
                throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function 'VALUES'");
            if (!c.env->insRow) return Value();
            int ci = c.env->insTable->find(e.args[0]->name);
            if (ci < 0) throw SqlError(1054, "42S22", "Unknown column '" + e.args[0]->name + "' in 'field list'");
            return (*c.env->insRow)[static_cast<size_t>(ci)];
        }
        if (n == "DATE_ADD" || n == "DATE_SUB" || n == "ADDDATE" || n == "SUBDATE") {
            if (e.args.size() != 2) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + n + "'");
            Value d = eval(*e.args[0], c);
            const Expr& iv = *e.args[1];
            std::string unit = "DAY";
            Value amt;
            if (iv.k == EK::Func && iv.name == "INTERVAL") { amt = eval(*iv.args[0], c); unit = iv.table; }
            else amt = eval(iv, c);
            if (d.isNull() || amt.isNull()) return Value();
            return addInterval(d, amt.asInt(), unit, (n == "DATE_SUB" || n == "SUBDATE") ? -1 : 1);
        }
        std::vector<Value> a;
        for (const auto& x : e.args) a.push_back(eval(*x, c));
        auto chk = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi)
                throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + n + "'");
            };
        auto anyNull = [&]() { for (const Value& v : a) if (v.isNull()) return true; return false; };
        if (n == "NOW" || n == "CURRENT_TIMESTAMP" || n == "SYSDATE") return Value::text(nowStr(false, c.env->now));
        if (n == "CURDATE") return Value::text(nowStr(true, c.env->now));
        if (n == "DATABASE") return c.env->db.empty() ? Value() : Value::text(c.env->db);
        if (n == "USER" || n == "CURRENT_USER" || n == "SESSION_USER" || n == "SYSTEM_USER") return Value::text("root@localhost");
        if (n == "CONNECTION_ID") return Value::integer(1);
        if (n == "VERSION") return Value::text("8.0.0-mysqlclone");
        if (n == "LAST_INSERT_ID") return Value::integer(c.env->lastId);
        if (n == "COALESCE") { for (const Value& v : a) if (!v.isNull()) return v; return Value(); }
        if (n == "IFNULL") { chk(2, 2); return a[0].isNull() ? a[1] : a[0]; }
        if (n == "NULLIF") { chk(2, 2); return (!a[0].isNull() && !a[1].isNull() && compareValues(a[0], a[1]) == 0) ? Value() : a[0]; }
        if (n == "IF") { chk(3, 3); return a[0].truthy() ? a[1] : a[2]; }
        if (n == "CONCAT") { if (anyNull()) return Value(); std::string r; for (const Value& v : a) r += v.str(); return Value::text(r); }
        if (n == "GREATEST" || n == "LEAST") {
            chk(1, 100);
            if (anyNull()) return Value();
            Value best = a[0];
            for (const Value& v : a) { int k = compareValues(v, best); if ((n == "GREATEST" && k > 0) || (n == "LEAST" && k < 0)) best = v; }
            return best;
        }
        if (n == "DATEDIFF") {
            chk(2, 2);
            DT x, y;
            bool tx = false, ty = false;
            if (anyNull() || !parseDT(a[0].str(), x, tx) || !parseDT(a[1].str(), y, ty)) return Value();
            return Value::integer(daysFromCivil(x.y, x.mo, x.d) - daysFromCivil(y.y, y.mo, y.d));
        }
        if (n == "MOD") { chk(2, 2); if (anyNull() || a[1].asReal() == 0) return Value(); return a[0].t == Value::T::Int && a[1].t == Value::T::Int ? Value::integer(a[0].i % a[1].i) : Value::real(std::fmod(a[0].asReal(), a[1].asReal())); }
        if (n == "POW" || n == "POWER") { chk(2, 2); if (anyNull()) return Value(); return Value::real(std::pow(a[0].asReal(), a[1].asReal())); }
        if (n == "REPLACE") {
            chk(3, 3);
            if (anyNull()) return Value();
            std::string s = a[0].str(), f = a[1].str(), t = a[2].str(), r;
            if (f.empty()) return Value::text(s);
            size_t i = 0;
            for (;;) { size_t j = s.find(f, i); if (j == std::string::npos) { r += s.substr(i); break; } r += s.substr(i, j - i) + t; i = j + f.size(); }
            return Value::text(r);
        }
        if (n == "SUBSTRING" || n == "SUBSTR") {
            chk(2, 3);
            if (anyNull()) return Value();
            std::string s = a[0].str();
            int64_t len = static_cast<int64_t>(s.size()), pos = a[1].asInt();
            int64_t st = pos > 0 ? pos - 1 : (pos < 0 ? len + pos : len);
            if (st < 0 || st >= len) return Value::text("");
            int64_t cnt = a.size() == 3 ? a[2].asInt() : len;
            if (cnt <= 0) return Value::text("");
            return Value::text(s.substr(static_cast<size_t>(st), static_cast<size_t>(cnt)));
        }
        if (n == "LEFT" || n == "RIGHT" || n == "REPEAT") {
            chk(2, 2);
            if (anyNull()) return Value();
            std::string s = a[0].str();
            int64_t k = std::max<int64_t>(0, a[1].asInt());
            if (n == "REPEAT") { std::string r; for (int64_t i = 0; i < k; ++i) r += s; return Value::text(r); }
            size_t kk = std::min(static_cast<size_t>(k), s.size());
            return Value::text(n == "LEFT" ? s.substr(0, kk) : s.substr(s.size() - kk));
        }
        // ---- additional string / math / date functions ----
        auto cut = [&](const std::string& str, size_t chars) {  // first `chars` UTF-8 characters
            size_t i = 0, k = 0;
            while (i < str.size() && k < chars) { ++i; while (i < str.size() && (static_cast<unsigned char>(str[i]) & 0xC0) == 0x80) ++i; ++k; }
            return str.substr(0, i);
            };
        auto lowerS = [](std::string str) { for (char& ch : str) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return str; };
        if (n == "CONCAT_WS") {
            chk(1, 1000);
            if (a[0].isNull()) return Value();
            std::string o, sep = a[0].str();
            bool first = true;
            for (size_t i = 1; i < a.size(); ++i) { if (a[i].isNull()) continue; if (!first) o += sep; o += a[i].str(); first = false; }
            return Value::text(o);
        }
        if (n == "INSTR" || n == "LOCATE") {
            chk(2, n == "INSTR" ? 2 : 3);
            if (anyNull()) return Value();
            std::string hay = lowerS(n == "INSTR" ? a[0].str() : a[1].str()), nd = lowerS(n == "INSTR" ? a[1].str() : a[0].str());
            int64_t from = n == "LOCATE" && a.size() == 3 ? a[2].asInt() : 1;
            if (from < 1 || static_cast<size_t>(from) - 1 > hay.size()) return Value::integer(0);
            size_t p = hay.find(nd, static_cast<size_t>(from) - 1);
            return Value::integer(p == std::string::npos ? 0 : static_cast<int64_t>(p) + 1);
        }
        if (n == "LPAD" || n == "RPAD") {
            chk(3, 3);
            if (anyNull() || a[1].asInt() < 0) return Value();
            std::string str = a[0].str(), pad = a[2].str();
            size_t want = static_cast<size_t>(a[1].asInt()), have = u8len(str);
            if (have >= want) return Value::text(cut(str, want));
            if (pad.empty()) return Value();
            std::string fill;
            while (u8len(fill) < want - have) fill += pad;
            fill = cut(fill, want - have);
            return Value::text(n == "LPAD" ? fill + str : str + fill);
        }
        if (n == "SPACE") { chk(1, 1); if (anyNull()) return Value(); return Value::text(std::string(static_cast<size_t>(std::max<int64_t>(0, a[0].asInt())), ' ')); }
        if (n == "ASCII") { chk(1, 1); if (anyNull()) return Value(); std::string str = a[0].str(); return Value::integer(str.empty() ? 0 : static_cast<unsigned char>(str[0])); }
        if (n == "STRCMP") { chk(2, 2); if (anyNull()) return Value(); return Value::integer(ciCompare(a[0].str(), a[1].str())); }
        if (n == "ISNULL") { chk(1, 1); return Value::integer(a[0].isNull() ? 1 : 0); }
        if (n == "FIELD") {
            chk(2, 1000);
            if (a[0].isNull()) return Value::integer(0);
            for (size_t i = 1; i < a.size(); ++i) if (!a[i].isNull() && compareValues(a[0], a[i]) == 0) return Value::integer(static_cast<int64_t>(i));
            return Value::integer(0);
        }
        if (n == "FORMAT") {
            chk(2, 2);
            if (anyNull()) return Value();
            int64_t d = std::max<int64_t>(0, std::min<int64_t>(30, a[1].asInt()));
            char b[512];
            std::snprintf(b, sizeof b, "%.*f", static_cast<int>(d), std::round(a[0].asReal() * std::pow(10.0, static_cast<double>(d))) / std::pow(10.0, static_cast<double>(d)));
            std::string str = b, ip = str, fp;
            size_t dot = str.find('.');
            if (dot != std::string::npos) { ip = str.substr(0, dot); fp = str.substr(dot); }
            bool neg = !ip.empty() && ip[0] == '-';
            if (neg) ip.erase(0, 1);
            std::string g;
            for (size_t i = 0; i < ip.size(); ++i) { if (i && (ip.size() - i) % 3 == 0) g += ','; g += ip[i]; }
            return Value::text((neg ? "-" : "") + g + fp);
        }
        if (n == "SIGN") { chk(1, 1); if (anyNull()) return Value(); double v = a[0].asReal(); return Value::integer(v > 0 ? 1 : v < 0 ? -1 : 0); }
        if (n == "TRUNCATE") {
            chk(2, 2);
            if (anyNull()) return Value();
            int64_t d = a[1].asInt();
            if (a[0].t == Value::T::Int && d >= 0) return a[0];
            double m = std::pow(10.0, static_cast<double>(d)), r = std::trunc(a[0].asReal() * m) / m;
            if (a[0].t == Value::T::Int) return Value::integer(static_cast<int64_t>(r));
            return a[0].scale >= 0 ? Value::dec(r, std::max<int>(0, static_cast<int>(d))) : Value::real(r);
        }
        if (n == "EXP" || n == "SIN" || n == "COS" || n == "TAN" || n == "DEGREES" || n == "RADIANS" || n == "ASIN" || n == "ACOS") {
            chk(1, 1);
            if (anyNull()) return Value();
            double v = a[0].asReal();
            if ((n == "ASIN" || n == "ACOS") && (v < -1 || v > 1)) return Value();
            return Value::real(n == "EXP" ? std::exp(v) : n == "SIN" ? std::sin(v) : n == "COS" ? std::cos(v) : n == "TAN" ? std::tan(v) :
                n == "ASIN" ? std::asin(v) : n == "ACOS" ? std::acos(v) : n == "DEGREES" ? v * 180.0 / 3.14159265358979323846 : v * 3.14159265358979323846 / 180.0);
        }
        if (n == "ATAN" || n == "ATAN2") {
            chk(1, 2);
            if (anyNull()) return Value();
            return Value::real(a.size() == 2 ? std::atan2(a[0].asReal(), a[1].asReal()) : std::atan(a[0].asReal()));
        }
        if (n == "LN" || n == "LOG" || n == "LOG2" || n == "LOG10") {
            chk(1, n == "LOG" ? 2 : 1);
            if (anyNull()) return Value();
            if (a.size() == 2) {
                double b = a[0].asReal(), x2 = a[1].asReal();
                if (b <= 1 || x2 <= 0) return Value();
                return Value::real(std::log(x2) / std::log(b));
            }
            double v = a[0].asReal();
            if (v <= 0) return Value();
            return Value::real(n == "LOG2" ? std::log2(v) : n == "LOG10" ? std::log10(v) : std::log(v));
        }
        if (n == "PI") { chk(0, 0); return Value::real(3.14159265358979323846); }
        if (n == "DAYOFMONTH" || n == "DAYOFYEAR" || n == "WEEKDAY" || n == "DAYNAME" || n == "MONTHNAME" || n == "QUARTER" ||
            n == "LAST_DAY" || n == "TO_DAYS" || n == "DATE_FORMAT") {
            chk(1, n == "DATE_FORMAT" ? 2 : 1);
            DT t;
            bool ht = false;
            if (a[0].isNull() || !parseDT(a[0].str(), t, ht)) return Value();
            static const char* dn[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
            static const char* mn[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
            int64_t z = daysFromCivil(t.y, t.mo, t.d);
            int wdSun = static_cast<int>(((z % 7 + 7) % 7 + 4) % 7);  // 0 = Sunday
            if (n == "DAYOFMONTH") return Value::integer(t.d);
            if (n == "DAYOFYEAR") return Value::integer(z - daysFromCivil(t.y, 1, 1) + 1);
            if (n == "WEEKDAY") return Value::integer((wdSun + 6) % 7);
            if (n == "DAYNAME") return Value::text(dn[wdSun]);
            if (n == "MONTHNAME") return Value::text(mn[t.mo - 1]);
            if (n == "QUARTER") return Value::integer((t.mo - 1) / 3 + 1);
            if (n == "LAST_DAY") return Value::text(fmtDT(DT{ t.y, t.mo, daysInMonth(t.y, t.mo), 0, 0, 0 }, false));
            if (n == "TO_DAYS") return Value::integer(z + 719528);
            if (a[1].isNull()) return Value();
            std::string f = a[1].str(), o;
            char b[16];
            auto num = [&](int v, int w) { std::snprintf(b, sizeof b, "%0*d", w, v); o += b; };
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i] != '%' || i + 1 == f.size()) { o += f[i]; continue; }
                char c = f[++i];
                int h12 = t.h % 12 == 0 ? 12 : t.h % 12;
                switch (c) {
                case 'Y': num(t.y, 4); break;
                case 'y': num(t.y % 100, 2); break;
                case 'm': num(t.mo, 2); break;
                case 'c': o += std::to_string(t.mo); break;
                case 'd': num(t.d, 2); break;
                case 'e': o += std::to_string(t.d); break;
                case 'H': num(t.h, 2); break;
                case 'k': o += std::to_string(t.h); break;
                case 'h': case 'I': num(h12, 2); break;
                case 'l': o += std::to_string(h12); break;
                case 'i': num(t.mi, 2); break;
                case 's': case 'S': num(t.s, 2); break;
                case 'p': o += t.h < 12 ? "AM" : "PM"; break;
                case 'M': o += mn[t.mo - 1]; break;
                case 'b': o += std::string(mn[t.mo - 1]).substr(0, 3); break;
                case 'W': o += dn[wdSun]; break;
                case 'a': o += std::string(dn[wdSun]).substr(0, 3); break;
                case 'w': o += std::to_string(wdSun); break;
                case 'j': num(static_cast<int>(z - daysFromCivil(t.y, 1, 1) + 1), 3); break;
                case 'T': num(t.h, 2); o += ':'; num(t.mi, 2); o += ':'; num(t.s, 2); break;
                case '%': o += '%'; break;
                default: o += c; break;
                }
            }
            return Value::text(o);
        }
        if (n == "FROM_DAYS") {
            chk(1, 1);
            if (anyNull()) return Value();
            int y, m, d;
            civilFromDays(a[0].asInt() - 719528, y, m, d);
            return Value::text(fmtDT(DT{ y, m, d, 0, 0, 0 }, false));
        }
        if (n == "TIMESTAMPDIFF" || n == "TIMESTAMPADD") {
            chk(3, 3);
            if (a[1].isNull() || a[2].isNull() || a[0].isNull()) return Value();
            std::string unit = a[0].str();
            if (n == "TIMESTAMPADD") return addInterval(a[2], a[1].asInt(), unit, 1);
            DT x, y;
            bool tx = false, ty = false;
            if (!parseDT(a[1].str(), x, tx) || !parseDT(a[2].str(), y, ty)) return Value();
            auto secs = [&](const DT& t) { return daysFromCivil(t.y, t.mo, t.d) * 86400 + static_cast<int64_t>(t.h) * 3600 + t.mi * 60 + t.s; };
            if (unit == "MONTH" || unit == "QUARTER" || unit == "YEAR") {
                int64_t months = (static_cast<int64_t>(y.y) * 12 + y.mo) - (static_cast<int64_t>(x.y) * 12 + x.mo);
                auto rest = [&](const DT& t) { return std::make_tuple(t.d, t.h, t.mi, t.s); };
                if (months > 0 && rest(y) < rest(x)) --months;
                else if (months < 0 && rest(y) > rest(x)) ++months;
                return Value::integer(unit == "MONTH" ? months : unit == "QUARTER" ? months / 3 : months / 12);
            }
            int64_t d = secs(y) - secs(x);
            int64_t div = unit == "DAY" ? 86400 : unit == "WEEK" ? 604800 : unit == "HOUR" ? 3600 : unit == "MINUTE" ? 60 : unit == "SECOND" ? 1 : 0;
            if (!div) throw SqlError(1064, "42000", "You have an error in your SQL syntax; unsupported unit " + unit);
            return Value::integer(d / div);
        }
        if (n == "EXTRACT") {
            chk(2, 2);
            DT t;
            bool ht = false;
            if (a[1].isNull() || !parseDT(a[1].str(), t, ht)) return Value();
            const std::string u = a[0].str();
            if (u == "YEAR") return Value::integer(t.y);
            if (u == "MONTH") return Value::integer(t.mo);
            if (u == "DAY") return Value::integer(t.d);
            if (u == "HOUR") return Value::integer(t.h);
            if (u == "MINUTE") return Value::integer(t.mi);
            if (u == "SECOND") return Value::integer(t.s);
            if (u == "QUARTER") return Value::integer((t.mo - 1) / 3 + 1);
            if (u == "YEAR_MONTH") return Value::integer(static_cast<int64_t>(t.y) * 100 + t.mo);
            throw SqlError(1064, "42000", "You have an error in your SQL syntax; unsupported EXTRACT unit " + u);
        }
        if (n == "UNIX_TIMESTAMP") {
            chk(0, 1);
            if (a.empty()) return Value::integer(static_cast<int64_t>(c.env->now));
            DT t;
            bool ht = false;
            if (a[0].isNull() || !parseDT(a[0].str(), t, ht)) return Value();
            std::tm tmv{};
            tmv.tm_year = t.y - 1900; tmv.tm_mon = t.mo - 1; tmv.tm_mday = t.d; tmv.tm_hour = t.h; tmv.tm_min = t.mi; tmv.tm_sec = t.s; tmv.tm_isdst = -1;
            return Value::integer(static_cast<int64_t>(std::mktime(&tmv)));
        }
        if (n == "FROM_UNIXTIME") {
            chk(1, 1);
            if (anyNull() || a[0].asInt() < 0) return Value();
            std::time_t tt = static_cast<std::time_t>(a[0].asInt());
            std::tm tmv{};
            localtime_r(&tt, &tmv);
            char b[32];
            std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
            return Value::text(b);
        }
        if (n == "STR_TO_DATE") {
            chk(2, 2);
            if (anyNull()) return Value();
            const std::string s = a[0].str(), f = a[1].str();
            static const char* mn[] = { "january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december" };
            DT t;
            t.y = 0; t.mo = 0; t.d = 0;
            bool hasTime = false, pm = false, hasP = false, h12 = false;
            size_t i = 0;
            auto number = [&](int maxDigits, int& out) {
                size_t st = i;
                while (i < s.size() && i - st < static_cast<size_t>(maxDigits) && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (i == st) return false;
                out = std::atoi(s.substr(st, i - st).c_str());
                return true;
                };
            for (size_t k = 0; k < f.size(); ++k) {
                if (f[k] != '%' || k + 1 == f.size()) {
                    if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(f[k]))) return Value();
                    ++i;
                    continue;
                }
                char sp = f[++k];
                int v = 0;
                switch (sp) {
                case 'Y': if (!number(4, v)) return Value(); t.y = v; break;
                case 'y': if (!number(2, v)) return Value(); t.y = v < 70 ? 2000 + v : 1900 + v; break;
                case 'm': case 'c': if (!number(2, v)) return Value(); t.mo = v; break;
                case 'd': case 'e': if (!number(2, v)) return Value(); t.d = v; break;
                case 'H': case 'k': if (!number(2, v)) return Value(); t.h = v; hasTime = true; break;
                case 'h': case 'I': case 'l': if (!number(2, v)) return Value(); t.h = v % 12; h12 = true; hasTime = true; break;
                case 'i': if (!number(2, v)) return Value(); t.mi = v; hasTime = true; break;
                case 's': case 'S': if (!number(2, v)) return Value(); t.s = v; hasTime = true; break;
                case 'p': {
                    if (i + 2 > s.size()) return Value();
                    std::string ap = lowerS(s.substr(i, 2));
                    if (ap != "am" && ap != "pm") return Value();
                    pm = ap == "pm"; hasP = true; i += 2; break;
                }
                case 'M': case 'b': {
                    bool found = false;
                    for (int q = 0; q < 12 && !found; ++q) {
                        std::string full = mn[q];
                        size_t len = sp == 'M' ? full.size() : 3;
                        if (i + len <= s.size() && lowerS(s.substr(i, len)) == full.substr(0, len)) { t.mo = q + 1; i += len; found = true; }
                    }
                    if (!found) return Value();
                    break;
                }
                case 'W': case 'a': while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i; break;
                case '%': if (i >= s.size() || s[i] != '%') return Value(); ++i; break;
                default: return Value();
                }
            }
            if (i != s.size()) return Value();
            if (h12 && hasP && pm) t.h += 12;
            if (t.mo < 0 || t.mo > 12 || t.d < 0 || t.d > 31 || t.h > 23 || t.mi > 59 || t.s > 59) return Value();
            if (t.mo >= 1 && t.d > daysInMonth(t.y, t.mo)) return Value();
            return Value::text(fmtDT(t, hasTime));
        }
        if (n == "REGEXP_LIKE" || n == "NOT_REGEXP" || n == "REGEXP_REPLACE" || n == "REGEXP_SUBSTR" || n == "REGEXP_INSTR") {
            chk(2, n == "REGEXP_REPLACE" ? 3 : n == "REGEXP_LIKE" ? 3 : 2);
            if (anyNull()) return Value();
            bool icase = true;
            if (n == "REGEXP_LIKE" && a.size() == 3) for (char mt : a[2].str()) { if (mt == 'c') icase = false; else if (mt == 'i') icase = true; }
            static std::map<std::string, std::regex> cache;
            std::string key = (icase ? "i:" : "c:") + a[1].str();
            auto it = cache.find(key);
            if (it == cache.end()) {
                if (cache.size() > 256) cache.clear();
                try { it = cache.emplace(key, std::regex(a[1].str(), icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript)).first; }
                catch (const std::regex_error&) { throw SqlError(3692, "HY000", "Illegal argument to a regular expression."); }
            }
            const std::string subj = a[0].str();
            if (n == "REGEXP_REPLACE") return Value::text(std::regex_replace(subj, it->second, a.size() == 3 ? a[2].str() : std::string()));
            std::smatch m;
            bool found = std::regex_search(subj, m, it->second);
            if (n == "REGEXP_LIKE") return Value::integer(found ? 1 : 0);
            if (n == "NOT_REGEXP") return Value::integer(found ? 0 : 1);
            if (n == "REGEXP_SUBSTR") return found ? Value::text(m.str()) : Value();
            return Value::integer(found ? static_cast<int64_t>(m.position()) + 1 : 0);
        }
        if (n.rfind("JSON_", 0) == 0) {
            using jsn::J;
            auto doc = [&](size_t i) {
                J j;
                if (!jsn::parse(a[i].str(), j))
                    throw SqlError(3141, "22032", "Invalid JSON text in argument " + std::to_string(i + 1) + " to function " + n + ": \"Invalid value.\" at position 0.");
                return j;
                };
            auto path = [&](size_t i) {
                std::vector<jsn::Step> st;
                if (!jsn::parsePath(a[i].str(), st)) throw SqlError(3143, "42000", "Invalid JSON path expression. The error is around character position 1.");
                return st;
                };
            auto noWild = [&](const std::vector<jsn::Step>& st) {
                if (jsn::wildcard(st)) throw SqlError(3178, "42000", "In this situation, path expressions may not contain the * and ** tokens or an array range.");
                };
            if (n == "JSON_VALID") {
                chk(1, 1);
                if (a[0].isNull()) return Value();
                J t;
                return Value::integer(a[0].t == Value::T::Text && jsn::parse(a[0].s, t) ? 1 : 0);
            }
            if (n == "JSON_QUOTE") { chk(1, 1); if (a[0].isNull()) return Value(); std::string o; jsn::quote(a[0].str(), o); return Value::text(o); }
            if (n == "JSON_OBJECT") {
                if (a.size() % 2) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function 'JSON_OBJECT'");
                J o = J::obj();
                for (size_t i = 0; i < a.size(); i += 2) {
                    if (a[i].isNull()) throw SqlError(3158, "22032", "JSON documents may not contain NULL member names.");
                    jsn::objSet(o, a[i].str(), jvalOf(a[i + 1]));
                }
                return jvalue(o);
            }
            if (n == "JSON_ARRAY") { J o = J::arr(); for (const Value& v : a) o.a.push_back(jvalOf(v)); return jvalue(o); }
            if (n == "JSON_UNQUOTE") {
                chk(1, 1);
                if (a[0].isNull()) return Value();
                J j;
                if (a[0].t == Value::T::Text && !a[0].s.empty() && a[0].s[0] == '"' && jsn::parse(a[0].s, j) && j.t == J::T::Str) return Value::text(j.s);
                if (a[0].json && jsn::parse(a[0].s, j) && j.t == J::T::Str) return Value::text(j.s);
                return Value::text(a[0].str());
            }
            if (a.empty() || a[0].isNull()) { chk(1, 1000); return Value(); }
            if (n == "JSON_TYPE") {
                chk(1, 1);
                J j = doc(0);
                static const char* names[] = { "NULL", "BOOLEAN", "INTEGER", "DOUBLE", "STRING", "ARRAY", "OBJECT" };
                return Value::text(names[static_cast<int>(j.t)]);
            }
            if (n == "JSON_EXTRACT") {
                chk(2, 1000);
                J j = doc(0);
                std::vector<const J*> hits;
                bool wc = false;
                for (size_t i = 1; i < a.size(); ++i) {
                    if (a[i].isNull()) return Value();
                    auto st = path(i);
                    wc = wc || jsn::wildcard(st);
                    jsn::select(j, st, 0, hits);
                }
                if (hits.empty()) return Value();
                if (a.size() == 2 && !wc) return jvalue(*hits[0]);
                J arr = J::arr();
                for (const J* h : hits) arr.a.push_back(*h);
                return jvalue(arr);
            }
            if (n == "JSON_LENGTH" || n == "JSON_KEYS") {
                chk(1, 2);
                J j = doc(0);
                const J* tgt = &j;
                if (a.size() == 2) {
                    if (a[1].isNull()) return Value();
                    std::vector<const J*> hits;
                    jsn::select(j, path(1), 0, hits);
                    if (hits.empty()) return Value();
                    tgt = hits[0];
                }
                if (n == "JSON_LENGTH") return Value::integer(tgt->t == J::T::Arr ? static_cast<int64_t>(tgt->a.size()) : tgt->t == J::T::Obj ? static_cast<int64_t>(tgt->o.size()) : 1);
                if (tgt->t != J::T::Obj) return Value();
                J ks = J::arr();
                for (const auto& m : tgt->o) ks.a.push_back(J::str(m.first));
                return jvalue(ks);
            }
            if (n == "JSON_CONTAINS") {
                chk(2, 3);
                if (a[1].isNull()) return Value();
                J j = doc(0), cand = a[1].t == Value::T::Text ? doc(1) : jvalOf(a[1]);
                const J* tgt = &j;
                if (a.size() == 3) {
                    if (a[2].isNull()) return Value();
                    std::vector<const J*> hits;
                    jsn::select(j, path(2), 0, hits);
                    if (hits.empty()) return Value();
                    tgt = hits[0];
                }
                return Value::integer(jsn::contains(*tgt, cand) ? 1 : 0);
            }
            if (n == "JSON_CONTAINS_PATH") {
                chk(3, 1000);
                if (a[1].isNull()) return Value();
                std::string mode = lowerS(a[1].str());
                if (mode != "one" && mode != "all") throw SqlError(3130, "42000", "Invalid JSON path expression: the oneOrAll argument may take these values: 'one' or 'all'.");
                J j = doc(0);
                int found = 0;
                for (size_t i = 2; i < a.size(); ++i) {
                    if (a[i].isNull()) return Value();
                    std::vector<const J*> hits;
                    jsn::select(j, path(i), 0, hits);
                    if (!hits.empty()) ++found;
                }
                return Value::integer(mode == "one" ? (found > 0) : (found == static_cast<int>(a.size()) - 2));
            }
            if (n == "JSON_SET" || n == "JSON_INSERT" || n == "JSON_REPLACE") {
                chk(3, 1000);
                if (a.size() % 2 == 0) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + n + "'");
                J j = doc(0);
                const int mode = n == "JSON_SET" ? 0 : n == "JSON_INSERT" ? 1 : 2;
                for (size_t i = 1; i + 1 < a.size(); i += 2) {
                    if (a[i].isNull()) return Value();
                    auto st = path(i);
                    noWild(st);
                    jsn::assign(j, st, jvalOf(a[i + 1]), mode);
                }
                return jvalue(j);
            }
            if (n == "JSON_REMOVE") {
                chk(2, 1000);
                J j = doc(0);
                for (size_t i = 1; i < a.size(); ++i) {
                    if (a[i].isNull()) return Value();
                    auto st = path(i);
                    noWild(st);
                    if (st.empty()) throw SqlError(3153, "42000", "Invalid JSON path expression. The path may not be '$'.");
                    jsn::removeAt(j, st);
                }
                return jvalue(j);
            }
            if (n == "JSON_ARRAY_APPEND") {
                chk(3, 1000);
                if (a.size() % 2 == 0) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + n + "'");
                J j = doc(0);
                for (size_t i = 1; i + 1 < a.size(); i += 2) {
                    if (a[i].isNull()) return Value();
                    auto st = path(i);
                    noWild(st);
                    std::vector<const J*> hits;
                    jsn::select(j, st, 0, hits);
                    if (hits.empty()) continue;
                    J* tg = const_cast<J*>(hits[0]);
                    if (tg->t == J::T::Arr) tg->a.push_back(jvalOf(a[i + 1]));
                    else { J w = J::arr(); w.a.push_back(*tg); w.a.push_back(jvalOf(a[i + 1])); *tg = std::move(w); }
                }
                return jvalue(j);
            }
            if (n == "JSON_MERGE_PATCH") {
                chk(2, 1000);
                std::function<J(J, const J&)> patch = [&](J target, const J& p) -> J {
                    if (p.t != J::T::Obj) return p;
                    if (target.t != J::T::Obj) target = J::obj();
                    for (const auto& m : p.o) {
                        if (m.second.t == J::T::Null) { jsn::removeAt(target, { jsn::Step{ jsn::Step::K::Key, m.first, 0 } }); continue; }
                        J cur;
                        for (const auto& tm : target.o) if (tm.first == m.first) cur = tm.second;
                        jsn::objSet(target, m.first, patch(cur, m.second));
                    }
                    return target;
                    };
                J acc = doc(0);
                for (size_t i = 1; i < a.size(); ++i) { if (a[i].isNull()) return Value(); acc = patch(acc, doc(i)); }
                return jvalue(acc);
            }
            throw SqlError(1305, "42000", "FUNCTION " + n + " does not exist");
        }
        // single-argument functions below
        chk(1, n == "ROUND" ? 2 : 1);
        if (a[0].isNull() || (a.size() > 1 && a[1].isNull())) return Value();
        const Value& x = a[0];
        if (n == "UPPER" || n == "UCASE" || n == "LOWER" || n == "LCASE") {
            std::string s = x.str();
            bool up = n[0] == 'U';
            for (char& ch : s) ch = static_cast<char>(up ? std::toupper(static_cast<unsigned char>(ch)) : std::tolower(static_cast<unsigned char>(ch)));
            return Value::text(s);
        }
        if (n == "LENGTH") return Value::integer(static_cast<int64_t>(x.str().size()));
        if (n == "CHAR_LENGTH" || n == "CHARACTER_LENGTH") return Value::integer(static_cast<int64_t>(u8len(x.str())));
        if (n == "REVERSE") { std::string s = x.str(); std::reverse(s.begin(), s.end()); return Value::text(s); }
        if (n == "TRIM" || n == "LTRIM" || n == "RTRIM") {
            std::string s = x.str();
            size_t b = 0, en = s.size();
            if (n != "RTRIM") while (b < en && s[b] == ' ') ++b;
            if (n != "LTRIM") while (en > b && s[en - 1] == ' ') --en;
            return Value::text(s.substr(b, en - b));
        }
        if (n == "ABS") {
            if (x.t == Value::T::Int) return Value::integer(std::llabs(x.i));
            return exactScale(x) >= 0 ? Value::dec(std::fabs(x.d), x.scale) : Value::real(std::fabs(x.asReal()));
        }
        if (n == "FLOOR") return Value::integer(static_cast<int64_t>(std::floor(x.asReal())));
        if (n == "CEIL" || n == "CEILING") return Value::integer(static_cast<int64_t>(std::ceil(x.asReal())));
        if (n == "SQRT") return x.asReal() < 0 ? Value() : Value::real(std::sqrt(x.asReal()));
        if (n == "ROUND") {
            int64_t d = a.size() > 1 ? a[1].asInt() : 0;
            if (x.t == Value::T::Int && d >= 0) return x;
            double p = std::pow(10.0, static_cast<double>(d)), r = std::round(x.asReal() * p) / p;
            if (d <= 0) return Value::integer(static_cast<int64_t>(r));
            return exactScale(x) >= 0 ? Value::dec(r, static_cast<int>(std::min<int64_t>(d, 30))) : Value::real(r);
        }
        if (n == "DATE" || n == "DAYOFWEEK" || n == "HOUR" || n == "MINUTE" || n == "SECOND") {
            DT t;
            bool ht = false;
            if (!parseDT(x.str(), t, ht)) return Value();
            if (n == "DATE") return Value::text(fmtDT(t, false));
            if (n == "HOUR") return Value::integer(t.h);
            if (n == "MINUTE") return Value::integer(t.mi);
            if (n == "SECOND") return Value::integer(t.s);
            int64_t z = daysFromCivil(t.y, t.mo, t.d);
            return Value::integer(((z % 7 + 7) % 7 + 4) % 7 + 1);  // 1 = Sunday
        }
        if (n == "YEAR" || n == "MONTH" || n == "DAY") {
            std::string s = x.str();
            if (s.size() < 10) return Value();
            return Value::integer(std::atoll(n == "YEAR" ? s.substr(0, 4).c_str() : n == "MONTH" ? s.substr(5, 2).c_str() : s.substr(8, 2).c_str()));
        }

        throw SqlError(1305, "42000", "FUNCTION " + n + " does not exist");
    }

    Value eval(const Expr& e, const Ctx& c) {
        switch (e.k) {
        case EK::Lit: return e.v;
        case EK::Col: {
            if (e.slot >= 0) {  // already resolved: walk up `level` enclosing contexts
                const Ctx* cx = &c;
                for (int i = 0; i < e.level; ++i) cx = cx->outer;
                return cx->row ? (*cx->row)[static_cast<size_t>(e.slot)] : Value();
            }
            const Ctx* cx = &c;
            for (int lvl = 0;; ++lvl) {
                if (cx->scope) {
                    try {
                        size_t idx = resolveCol(e, *cx->scope);
                        e.level = lvl;
                        if (lvl > 0) ++c.env->outerRefs;
                        return cx->row ? (*cx->row)[idx] : Value();
                    }
                    catch (const SqlError& er) {
                        if (er.code != 1054) throw;
                        if (lvl == 0 && c.items && e.table.empty())
                            for (const SelItem& it : *c.items)
                                if (it.e && !it.alias.empty() && ciCompare(it.alias, e.name) == 0) return eval(*it.e, c);
                    }
                }
                if (!cx->outer)
                    throw SqlError(1054, "42S22", "Unknown column '" + (e.table.empty() ? "" : e.table + ".") + e.name + "' in 'field list'");
                cx = cx->outer;
            }
        }
        case EK::Unary: {
            Value v = eval(*e.args[0], c);
            if (v.isNull()) return v;
            if (e.name == "NOT") return B(!v.truthy());
            if (v.t == Value::T::Int) return Value::integer(-v.i);
            return exactScale(v) >= 0 ? Value::dec(-v.d, v.scale) : Value::real(-v.asReal());
        }
        case EK::IsNull: { bool n = eval(*e.args[0], c).isNull(); return B(e.neg ? !n : n); }
        case EK::In: {
            Value v = eval(*e.args[0], c);
            if (v.isNull()) return v;
            std::vector<Value> cand;
            if (e.sub) { auto sr = subRows(e, c); for (const Row& r : *sr) cand.push_back(r.empty() ? Value() : r[0]); }
            else for (size_t i = 1; i < e.args.size(); ++i) cand.push_back(eval(*e.args[i], c));
            bool sawNull = false, hit = false;
            CType ct = colType(*e.args[0], c);
            for (const Value& w0 : cand) {
                if (w0.isNull()) { sawNull = true; continue; }
                Value vv = v, w = w0;
                alignDates(ct, vv, w);
                if (compareValues(vv, w) == 0) { hit = true; break; }
            }
            if (hit) return B(!e.neg);
            if (sawNull) return Value();
            return B(e.neg);
        }
        case EK::Sub: {
            auto rp = subRows(e, c);
            const auto& rows = *rp;
            if (rows.empty()) return Value();
            if (rows.size() > 1) throw SqlError(1242, "21000", "Subquery returns more than 1 row");
            if (rows[0].size() != 1) throw SqlError(1241, "21000", "Operand should contain 1 column(s)");
            return rows[0][0];
        }
        case EK::Exists: return B(!subRows(e, c)->empty());
        case EK::Param: throw SqlError(1210, "HY000", "Incorrect arguments to mysqld_stmt_execute");
        case EK::Between: {
            Value v = eval(*e.args[0], c), lo = eval(*e.args[1], c), hi = eval(*e.args[2], c);
            if (v.isNull() || lo.isNull() || hi.isNull()) return Value();
            CType ct = colType(*e.args[0], c);
            Value v1 = v, v2 = v;
            alignDates(ct, v1, lo);
            alignDates(ct, v2, hi);
            bool in = compareValues(v1, lo) >= 0 && compareValues(v2, hi) <= 0;
            return B(e.neg ? !in : in);
        }
        case EK::Like: {
            Value v = eval(*e.args[0], c), p = eval(*e.args[1], c);
            if (v.isNull() || p.isNull()) return Value();
            char esc = '\\';
            if (e.args.size() > 2) {
                Value ev = eval(*e.args[2], c);
                if (ev.isNull() || ev.str().size() > 1) throw SqlError(1210, "HY000", "Incorrect arguments to ESCAPE");
                esc = ev.str().empty() ? '\0' : ev.str()[0];
            }
            bool m = likeMatch(v.str(), 0, p.str(), 0, esc);
            return B(e.neg ? !m : m);
        }
        case EK::Case: {
            size_t i = 0;
            Value op;
            if (e.hasOperand) op = eval(*e.args[i++], c);
            size_t end = e.args.size() - (e.hasElse ? 1 : 0);
            for (; i + 1 < end + 1 && i < end; i += 2) {
                Value w = eval(*e.args[i], c);
                bool ok = e.hasOperand ? (!op.isNull() && !w.isNull() && compareValues(op, w) == 0) : w.truthy();
                if (ok) return eval(*e.args[i + 1], c);
            }
            return e.hasElse ? eval(*e.args.back(), c) : Value();
        }
        case EK::Func:
            if (e.win) {
                if (c.env->curWin) for (const auto& pv : *c.env->curWin) if (pv.first == &e) return pv.second;
                throw SqlError(3593, "HY000", "You cannot use the window function '" + e.name + "' in this context.");
            }
            if (isAgg(e.name)) return evalAgg(e, c);
            if (e.name == "DEFAULT") throw SqlError(1064, "42000", "DEFAULT is only valid in INSERT VALUES");
            return scalarFunc(e, c);
        case EK::Bin: break;
        }
        const std::string& op = e.name;
        if (op == "AND") {
            Value l = eval(*e.args[0], c);
            if (!l.isNull() && !l.truthy()) return B(false);
            Value r = eval(*e.args[1], c);
            if (!r.isNull() && !r.truthy()) return B(false);
            return (l.isNull() || r.isNull()) ? Value() : B(true);
        }
        if (op == "OR") {
            Value l = eval(*e.args[0], c);
            if (l.truthy()) return B(true);
            Value r = eval(*e.args[1], c);
            if (r.truthy()) return B(true);
            return (l.isNull() || r.isNull()) ? Value() : B(false);
        }
        if ((op == "+" || op == "-") && e.args[1]->k == EK::Func && e.args[1]->name == "INTERVAL") {  // date +/- INTERVAL n UNIT
            Value d = eval(*e.args[0], c), amt = eval(*e.args[1]->args[0], c);
            if (d.isNull() || amt.isNull()) return Value();
            return addInterval(d, amt.asInt(), e.args[1]->table, op == "-" ? -1 : 1);
        }
        Value l = eval(*e.args[0], c), r = eval(*e.args[1], c);
        if (op == "<=>") {
            if (l.isNull() || r.isNull()) return B(l.isNull() && r.isNull());
            return B(compareValues(l, r) == 0);
        }
        if (l.isNull() || r.isNull()) return Value();
        if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (e.args[0]->k == EK::Col) alignDates(colType(*e.args[0], c), l, r);
            else if (e.args[1]->k == EK::Col) alignDates(colType(*e.args[1], c), r, l);
        }
        if (op == "=") return B(compareValues(l, r) == 0);
        if (op == "<>") return B(compareValues(l, r) != 0);
        if (op == "<") return B(compareValues(l, r) < 0);
        if (op == "<=") return B(compareValues(l, r) <= 0);
        if (op == ">") return B(compareValues(l, r) > 0);
        if (op == ">=") return B(compareValues(l, r) >= 0);
        bool ints = l.t == Value::T::Int && r.t == Value::T::Int;
        int sl = exactScale(l), sr = exactScale(r);
        bool exact = sl >= 0 && sr >= 0;  // INT/DECIMAL operands -> DECIMAL result; anything else -> DOUBLE
        auto num = [&](double v, int sc) { return exact ? Value::dec(v, std::min(sc, 30)) : Value::real(v); };
        if (op == "+") return ints ? Value::integer(l.i + r.i) : num(l.asReal() + r.asReal(), std::max(sl, sr));
        if (op == "-") return ints ? Value::integer(l.i - r.i) : num(l.asReal() - r.asReal(), std::max(sl, sr));
        if (op == "*") return ints ? Value::integer(l.i * r.i) : num(l.asReal() * r.asReal(), sl + sr);
        if (r.asReal() == 0) return Value();  // division by zero -> NULL
        if (op == "/") return num(l.asReal() / r.asReal(), sl + 4);  // div_precision_increment = 4
        if (op == "DIV") return Value::integer(static_cast<int64_t>(std::trunc(l.asReal() / r.asReal())));
        if (op == "%") return ints ? Value::integer(l.i % r.i) : num(std::fmod(l.asReal(), r.asReal()), std::max(sl, sr));
        throw SqlError(1064, "42000", "Unsupported operator " + op);
    }

    int cmpOrder(const Value& a, const Value& b) {
        if (a.isNull() || b.isNull()) return a.isNull() == b.isNull() ? 0 : (a.isNull() ? -1 : 1);
        return compareValues(a, b);
    }
    bool keysLess(const std::vector<Value>& a, const std::vector<Value>& b, const std::vector<OrderItem>& ord) {
        for (size_t i = 0; i < ord.size(); ++i) {
            int k = cmpOrder(a[i], b[i]);
            if (k != 0) return ord[i].desc ? k > 0 : k < 0;
        }
        return false;
    }

    Scope scopeFor(const Table& t, const std::string& alias) {
        Scope s;
        for (const Column& c : t.cols) s.cols.push_back({ alias, c.name, c.type });
        return s;
    }

    struct IndexPlan {
        bool used = false;
        size_t index = 0;
        std::string type = "ALL";  // const / ref / range / ALL
        std::vector<size_t> rows;  // candidate row positions, ascending
        std::vector<std::string> possible;
    };
    IndexPlan planIndex(const Table& t, const Scope& sc, const Expr* where, bool needQualified, const Env* env,
        const Ctx* outer, bool allowOuter);

    std::vector<size_t> matchRows(const Table& t, const Scope& sc, const Expr* where, const std::vector<OrderItem>& order,
        int64_t limit, const Env* env) {
        std::vector<size_t> idx;
        IndexPlan plan = planIndex(t, sc, where, false, env, nullptr, false);
        auto consider = [&](size_t i) {
            if (!where || eval(*where, Ctx{ &sc, &t.rows[i], nullptr, env, nullptr }).truthy()) idx.push_back(i);
            };
        if (plan.used) for (size_t i : plan.rows) consider(i);
        else for (size_t i = 0; i < t.rows.size(); ++i) consider(i);
        if (!order.empty()) {
            std::vector<std::pair<std::vector<Value>, size_t>> ks;
            for (size_t i : idx) {
                std::vector<Value> k;
                for (const auto& o : order) k.push_back(eval(*o.e, Ctx{ &sc, &t.rows[i], nullptr, env, nullptr }));
                ks.emplace_back(std::move(k), i);
            }
            std::stable_sort(ks.begin(), ks.end(), [&](const auto& x, const auto& y) { return keysLess(x.first, y.first, order); });
            for (size_t j = 0; j < idx.size(); ++j) idx[j] = ks[j].second;
        }
        if (limit >= 0 && idx.size() > static_cast<size_t>(limit)) idx.resize(static_cast<size_t>(limit));
        return idx;
    }

    Value coerce(const Column& c, const Value& v, size_t rowNo) {
        if (v.isNull()) return v;
        auto bad = [&](const char* kind) {
            return SqlError(1366, "HY000", std::string("Incorrect ") + kind + " value: '" + v.str() + "' for column '" + c.name + "' at row " + std::to_string(rowNo));
            };
        if (c.type == CType::Text && c.typeName == "json") {
            if (v.json) return v;
            if (v.t != Value::T::Text) { Value x = Value::text(jsn::dump(jvalOf(v))); x.json = true; return x; }
            jsn::J j;
            if (!jsn::parse(v.s, j))
                throw SqlError(3140, "22032", "Invalid JSON text: \"Invalid value.\" at position 0 in value for column '" + c.name + "'.");
            return jvalue(j);
        }
        if (c.type == CType::Text) {
            std::string s = v.str();
            if (c.len > 0 && u8len(s) > static_cast<size_t>(c.len))
                throw SqlError(1406, "22001", "Data too long for column '" + c.name + "' at row " + std::to_string(rowNo));
            return Value::text(s);
        }
        if (c.type == CType::Date || c.type == CType::DateTime) {
            std::string s = v.str();
            if (v.t == Value::T::Int) {  // YYYYMMDD / YYYYMMDDhhmmss
                std::string dg = std::to_string(v.i);
                if (dg.size() == 8) s = dg.substr(0, 4) + "-" + dg.substr(4, 2) + "-" + dg.substr(6, 2);
                else if (dg.size() == 14) s = dg.substr(0, 4) + "-" + dg.substr(4, 2) + "-" + dg.substr(6, 2) + " " + dg.substr(8, 2) + ":" + dg.substr(10, 2) + ":" + dg.substr(12, 2);
            }
            DT t;
            bool ht = false;
            bool dateOnly = c.type == CType::Date;
            if (!parseDT(s, t, ht))
                throw SqlError(1292, "22007", std::string("Incorrect ") + (dateOnly ? "date" : "datetime") + " value: '" + v.str() + "' for column '" + c.name + "' at row " + std::to_string(rowNo));
            return Value::text(fmtDT(t, !dateOnly));
        }
        double d = 0;
        if (v.t == Value::T::Text) {
            char* end = nullptr;
            d = std::strtod(v.s.c_str(), &end);
            while (end && *end == ' ') ++end;
            if (v.s.empty() || (end && *end != '\0')) throw bad(c.type == CType::Int ? "integer" : "double");
        }
        else d = v.asReal();
        int dp = 0, ds = 0;
        if (decParams(c.typeName, dp, ds)) {
            double m = std::pow(10.0, ds), r = std::round(d * m) / m;
            if (std::fabs(r) >= std::pow(10.0, dp - ds))
                throw SqlError(1264, "22003", "Out of range value for column '" + c.name + "' at row " + std::to_string(rowNo));
            return Value::dec(r, ds);
        }
        if (c.type == CType::Int) {
            int64_t iv = v.t == Value::T::Int ? v.i : static_cast<int64_t>(std::llround(d)), lo = 0, hi = 0;
            intRange(c.typeName, lo, hi);
            if (iv < lo || iv > hi) throw SqlError(1264, "22003", "Out of range value for column '" + c.name + "' at row " + std::to_string(rowNo));
            return Value::integer(iv);
        }
        return Value::real(d);
    }

    SqlError dupError(const UniqueKey& u, const Row& r) {
        std::string key;
        for (size_t k = 0; k < u.cols.size(); ++k) { if (k) key += "-"; key += r[u.cols[k]].str(); }
        return SqlError(1062, "23000", "Duplicate entry '" + key + "' for key '" + u.name + "'");
    }

    int lookup(const Expr& e, const Scope& sc) {
        int f = -1;
        for (size_t i = 0; i < sc.cols.size(); ++i) {
            if (ciCompare(sc.cols[i].name, e.name) != 0) continue;
            if (!e.table.empty() && sc.cols[i].table != e.table) continue;
            if (f >= 0) return -2;
            f = static_cast<int>(i);
        }
        return f;
    }

    // Finds an equality conjunct "left.col = right.col" (comparable types) usable for a hash join.
    void hashKeys(const Expr& on, const Scope& l, const Scope& r, int& lk, int& rk) {
        if (on.k == EK::Bin && on.name == "AND") {
            hashKeys(*on.args[0], l, r, lk, rk);
            if (lk < 0) hashKeys(*on.args[1], l, r, lk, rk);
            return;
        }
        if (on.k != EK::Bin || on.name != "=" || on.args[0]->k != EK::Col || on.args[1]->k != EK::Col) return;
        for (int swap = 0; swap < 2; ++swap) {
            const Expr& x = *on.args[swap ? 1 : 0];
            const Expr& y = *on.args[swap ? 0 : 1];
            int xi = lookup(x, l), yi = lookup(y, r);
            if (xi >= 0 && yi >= 0 && lookup(x, r) == -1 && lookup(y, l) == -1 &&
                textual(l.cols[static_cast<size_t>(xi)].type) == textual(r.cols[static_cast<size_t>(yi)].type)) {
                lk = xi;
                rk = yi;
                return;
            }
        }
    }

    // ---- index planning: pick an ordered index from sargable WHERE conjuncts (the full WHERE is re-checked afterwards) ----
    void conjuncts(const Expr& e, std::vector<const Expr*>& out) {
        if (e.k == EK::Bin && e.name == "AND") { conjuncts(*e.args[0], out); conjuncts(*e.args[1], out); }
        else out.push_back(&e);
    }

    // Constant w.r.t. this table: no column of `sc` (outer-query columns count as constants when allowed), no aggregate/subquery.
    bool isConstExpr(const Expr& e, const Scope& sc, bool allowOuter) {
        if (e.k == EK::Sub || e.k == EK::Exists || e.k == EK::Param) return false;
        if (e.k == EK::Col) return allowOuter && lookup(e, sc) == -1;
        if (e.k == EK::Func && isAgg(e.name)) return false;
        for (const auto& a : e.args) if (!isConstExpr(*a, sc, allowOuter)) return false;
        return true;
    }

    struct Pred { size_t col; std::string op; std::vector<Value> vals; };

    IndexPlan planIndex(const Table& t, const Scope& sc, const Expr* where, bool needQualified, const Env* env,
        const Ctx* outer, bool allowOuter) {
        IndexPlan plan;
        if (!where || t.nIndexes() == 0) return plan;
        std::vector<const Expr*> cj;
        conjuncts(*where, cj);
        std::vector<Pred> preds;
        auto colOf = [&](const Expr& x, size_t& ci) {
            if (x.k != EK::Col || (needQualified && x.table.empty())) return false;
            int i = lookup(x, sc);
            if (i < 0) return false;
            ci = static_cast<size_t>(i);
            return true;
            };
        auto constOf = [&](const Expr& x, size_t ci, Value& out) {
            if (!isConstExpr(x, sc, allowOuter)) return false;
            try { out = eval(x, Ctx{ nullptr, nullptr, nullptr, env, nullptr, outer }); }
            catch (const SqlError&) { return false; }
            if (out.isNull()) return false;
            CType ct = t.cols[ci].type;
            if (ct == CType::Date || ct == CType::DateTime) {
                if (out.t != Value::T::Text) return false;
                Value probe = Value::text("0000-00-00");
                alignDates(ct, probe, out);  // normalizes `out`; DATE column vs datetime literal changes `probe` -> not sargable
                return probe.s == "0000-00-00";
            }
            return (ct == CType::Int || ct == CType::Real) ? out.isNum() : out.t == Value::T::Text;
            };
        for (const Expr* x : cj) {
            size_t ci = 0;
            Value v;
            if (x->k == EK::Bin && (x->name == "=" || x->name == "<" || x->name == "<=" || x->name == ">" || x->name == ">=")) {
                if (colOf(*x->args[0], ci) && constOf(*x->args[1], ci, v)) preds.push_back({ ci, x->name,{ v } });
                else if (colOf(*x->args[1], ci) && constOf(*x->args[0], ci, v)) {
                    std::string op = x->name == "<" ? ">" : x->name == "<=" ? ">=" : x->name == ">" ? "<" : x->name == ">=" ? "<=" : "=";
                    preds.push_back({ ci, op,{ v } });
                }
            }
            else if (x->k == EK::Between && !x->neg && colOf(*x->args[0], ci)) {
                Value lo, hi;
                if (constOf(*x->args[1], ci, lo) && constOf(*x->args[2], ci, hi)) {
                    preds.push_back({ ci, ">=",{ lo } });
                    preds.push_back({ ci, "<=",{ hi } });
                }
            }
            else if (x->k == EK::In && !x->neg && !x->sub && colOf(*x->args[0], ci)) {
                std::vector<Value> vals;
                bool ok = true;
                for (size_t i = 1; i < x->args.size() && ok; ++i) { Value w; ok = constOf(*x->args[i], ci, w); vals.push_back(w); }
                if (ok && !vals.empty()) preds.push_back({ ci, "IN", vals });
            }
        }
        if (preds.empty()) return plan;
        t.ensureIndexes();

        struct Best { double score = 0; size_t i = 0; std::vector<Value> prefix, in; bool hasLo = false, hasHi = false, loInc = false, hiInc = false; Value lo, hi; bool full = false; } best;
        for (size_t i = 0; i < t.nIndexes(); ++i) {
            const UniqueKey& d = t.indexDef(i);
            Best c;
            c.i = i;
            while (c.prefix.size() < d.cols.size()) {
                const Pred* eq = nullptr;
                for (const Pred& p : preds) if (p.col == d.cols[c.prefix.size()] && p.op == "=") { eq = &p; break; }
                if (!eq) break;
                c.prefix.push_back(eq->vals[0]);
            }
            if (c.prefix.size() < d.cols.size()) {
                size_t col = d.cols[c.prefix.size()];
                for (const Pred& p : preds) {
                    if (p.col != col) continue;
                    if (p.op == ">" || p.op == ">=") { c.hasLo = true; c.loInc = p.op == ">="; c.lo = p.vals[0]; }
                    else if (p.op == "<" || p.op == "<=") { c.hasHi = true; c.hiInc = p.op == "<="; c.hi = p.vals[0]; }
                    else if (p.op == "IN" && c.prefix.empty() && c.in.empty()) c.in = p.vals;
                }
            }
            if (c.prefix.empty() && !c.hasLo && !c.hasHi && c.in.empty()) continue;
            plan.possible.push_back(d.name);
            c.full = c.prefix.size() == d.cols.size();
            c.score = static_cast<double>(c.prefix.size()) * 4 + (c.hasLo || c.hasHi ? 2 : 0) + (c.in.empty() ? 0 : 3) + (i < t.uniques.size() && c.full ? 100 : 0);
            if (c.score > best.score) best = c;
        }
        if (best.score <= 0) return plan;

        const IndexMap& m = t.idx[best.i];
        auto scan = [&](const std::vector<Value>& pre, bool useRange) {
            std::vector<Value> start = pre;
            if (useRange && best.hasLo) start.push_back(best.lo);
            for (auto it = m.lower_bound(start); it != m.end(); ++it) {
                const std::vector<Value>& key = it->first;
                bool same = true;
                for (size_t j = 0; j < pre.size(); ++j) if (compareValues(key[j], pre[j]) != 0) { same = false; break; }
                if (!same) break;
                if (useRange && (best.hasLo || best.hasHi)) {
                    const Value& v = key[pre.size()];
                    if (best.hasLo) { int cmp = compareValues(v, best.lo); if (cmp < 0 || (cmp == 0 && !best.loInc)) continue; }
                    if (best.hasHi) { int cmp = compareValues(v, best.hi); if (cmp > 0 || (cmp == 0 && !best.hiInc)) break; }
                }
                plan.rows.insert(plan.rows.end(), it->second.begin(), it->second.end());
            }
            };
        if (!best.in.empty()) {
            std::set<std::string> seen;
            for (const Value& v : best.in) if (seen.insert(keyOf(v)).second) scan({ v }, false);
            plan.type = "range";
        }
        else {
            scan(best.prefix, true);
            plan.type = (best.hasLo || best.hasHi) ? "range" : ((best.i < t.uniques.size() && best.full) ? "const" : "ref");
        }
        std::sort(plan.rows.begin(), plan.rows.end());
        plan.rows.erase(std::unique(plan.rows.begin(), plan.rows.end()), plan.rows.end());
        plan.used = true;
        plan.index = best.i;
        return plan;
    }

    Value defaultFor(const Column& c, std::time_t now) { return c.defNow ? Value::text(nowStr(false, now)) : c.def; }

    void addUnique(Table& t, const std::string& name, const std::vector<size_t>& cols) {
        UniqueKey d{ name, cols };
        IndexMap seen;
        for (const Row& r : t.rows) {
            std::vector<Value> k;
            if (t.makeKey(d, r, k) && !seen.emplace(k, std::vector<size_t>{0}).second) throw dupError(d, r);
        }
        t.uniques.push_back(d);
        t.invalidate();
    }

    std::string quoteLit(const std::string& s) {
        std::string r = "'";
        for (char ch : s) { if (ch == '\'' || ch == '\\') r += '\\'; r += ch; }
        return r + "'";
    }

    struct Unit { const Row* row; const std::vector<const Row*>* group; };

    // Evaluates one window function over all units (rows, or groups of a grouped query); result is indexed like `units`.
    std::vector<Value> computeWindow(const Expr& w, const Scope& sc, const std::vector<Unit>& units, const Env* env, const Ctx* outerCtx) {
        const WinSpec& ws = *w.win;
        const std::string& fn = w.name;
        const size_t n = units.size();
        std::vector<Value> out(n);
        auto uc = [&](size_t i) { return Ctx{ &sc, units[i].row, units[i].group, env, nullptr, outerCtx }; };
        static const std::set<std::string> rankFns = { "ROW_NUMBER", "RANK", "DENSE_RANK", "PERCENT_RANK", "CUME_DIST", "NTILE" };
        static const std::set<std::string> valueFns = { "LAG", "LEAD", "FIRST_VALUE", "LAST_VALUE", "NTH_VALUE" };
        const bool isRank = rankFns.count(fn) > 0, isValue = valueFns.count(fn) > 0;
        if (!isRank && !isValue && !isAgg(fn))
            throw SqlError(1305, "42000", "FUNCTION " + fn + " does not exist");
        if (ws.hasFrame && !ws.rows) {
            auto numeric = [](const FrameBound& b) { return b.k == FrameBound::K::Preceding || b.k == FrameBound::K::Following; };
            if (numeric(ws.lo) || numeric(ws.hi))
                throw SqlError(1235, "42000", "This version of MySQL doesn't yet support 'RANGE with numeric offsets'");
        }
        // partitions (first-seen order) and per-unit ORDER BY keys
        std::vector<std::vector<size_t>> parts;
        std::map<std::string, size_t> pidx;
        std::vector<std::vector<Value>> okeys(n);
        for (size_t i = 0; i < n; ++i) {
            Ctx c = uc(i);
            std::string key;
            for (const auto& p : ws.part) { key += keyOf(eval(*p, c)); key += '\x1f'; }
            auto it = pidx.find(key);
            if (it == pidx.end()) { it = pidx.emplace(key, parts.size()).first; parts.emplace_back(); }
            parts[it->second].push_back(i);
            for (const auto& o : ws.order) okeys[i].push_back(eval(*o.e, c));
        }
        // arguments, evaluated once per unit
        std::vector<Value> av(n);
        std::vector<Value> extra1(n), extra2(n);  // LAG/LEAD offset+default, NTILE/NTH_VALUE n
        const bool star = w.star;
        if (!star && !w.args.empty() && fn != "NTILE")
            for (size_t i = 0; i < n; ++i) av[i] = eval(*w.args[0], uc(i));
        if (w.args.size() > 1) for (size_t i = 0; i < n; ++i) extra1[i] = eval(*w.args[1], uc(i));
        if (w.args.size() > 2) for (size_t i = 0; i < n; ++i) extra2[i] = eval(*w.args[2], uc(i));
        if (fn == "NTILE") { if (w.args.size() != 1) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function 'NTILE'"); for (size_t i = 0; i < n; ++i) extra1[i] = eval(*w.args[0], uc(i)); }
        // aggregate windows reuse evalAgg over one-column rows holding the argument value
        std::vector<Row> argRows;
        Scope as1;
        ExprP colE;
        Expr aggE;
        if (isAgg(fn)) {
            argRows.resize(n);
            for (size_t i = 0; i < n; ++i) argRows[i] = Row{ star ? Value::integer(1) : av[i] };
            as1.cols.push_back({ "", "v", CType::Text });
            colE = std::make_unique<Expr>();
            colE->k = EK::Col; colE->name = "v";
            aggE.k = EK::Func; aggE.name = fn; aggE.distinct = w.distinct; aggE.star = star; aggE.table = w.table;
            if (!star) { auto ce = std::make_unique<Expr>(); ce->k = EK::Col; ce->name = "v"; aggE.args.push_back(std::move(ce)); }
        }
        for (auto& idx : parts) {
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return keysLess(okeys[a], okeys[b], ws.order); });
            const size_t m = idx.size();
            std::vector<size_t> pStart(m), pEnd(m);  // peer-group bounds (sorted positions)
            for (size_t k = 0; k < m; ++k) {
                bool newGroup = k == 0 || (!ws.order.empty() && (keysLess(okeys[idx[k - 1]], okeys[idx[k]], ws.order) || keysLess(okeys[idx[k]], okeys[idx[k - 1]], ws.order)));
                pStart[k] = newGroup ? k : pStart[k - 1];
            }
            for (size_t k = m; k-- > 0;) pEnd[k] = (k + 1 < m && pStart[k + 1] == pStart[k]) ? pEnd[k + 1] : k;
            auto frame = [&](size_t k, size_t& lo, size_t& hi) -> bool {
                int64_t l, h;
                const int64_t last = static_cast<int64_t>(m) - 1, kk = static_cast<int64_t>(k);
                if (!ws.hasFrame) { l = 0; h = ws.order.empty() ? last : static_cast<int64_t>(pEnd[k]); }
                else {
                    auto bound = [&](const FrameBound& b, bool isLo) -> int64_t {
                        switch (b.k) {
                        case FrameBound::K::UnboundedPreceding: return 0;
                        case FrameBound::K::UnboundedFollowing: return last;
                        case FrameBound::K::Preceding: return kk - b.n;
                        case FrameBound::K::Following: return kk + b.n;
                        case FrameBound::K::CurrentRow: break;
                        }
                        if (ws.rows) return kk;
                        return isLo ? static_cast<int64_t>(pStart[k]) : static_cast<int64_t>(pEnd[k]);
                        };
                    l = bound(ws.lo, true); h = bound(ws.hi, false);
                }
                l = std::max<int64_t>(l, 0); h = std::min<int64_t>(h, last);
                if (l > h) return false;
                lo = static_cast<size_t>(l); hi = static_cast<size_t>(h);
                return true;
                };
            int64_t dense = 0;
            size_t plo = 1, phi = 0;  // previous frame (plo > phi = none) for result reuse
            Value prev;
            for (size_t k = 0; k < m; ++k) {
                const size_t u = idx[k];
                Value& res = out[u];
                if (fn == "ROW_NUMBER") res = Value::integer(static_cast<int64_t>(k) + 1);
                else if (fn == "RANK") res = Value::integer(static_cast<int64_t>(pStart[k]) + 1);
                else if (fn == "DENSE_RANK") { if (k == 0 || pStart[k] == k) ++dense; res = Value::integer(dense); }
                else if (fn == "PERCENT_RANK") res = Value::real(m > 1 ? static_cast<double>(pStart[k]) / static_cast<double>(m - 1) : 0.0);
                else if (fn == "CUME_DIST") res = Value::real(static_cast<double>(pEnd[k] + 1) / static_cast<double>(m));
                else if (fn == "NTILE") {
                    const Value& nv = extra1[u];
                    if (nv.isNull() || nv.asInt() <= 0) throw SqlError(1210, "HY000", "Incorrect arguments to ntile");
                    int64_t b = nv.asInt(), mm = static_cast<int64_t>(m), kk = static_cast<int64_t>(k), size = mm / b, rem = mm % b;
                    res = Value::integer(kk < rem * (size + 1) ? kk / (size + 1) + 1 : rem + (kk - rem * (size + 1)) / size + 1);
                }
                else if (fn == "LAG" || fn == "LEAD") {
                    int64_t off = w.args.size() > 1 ? extra1[u].asInt() : 1;
                    if (off < 0) throw SqlError(1210, "HY000", "Incorrect arguments to " + fn);
                    int64_t tk = static_cast<int64_t>(k) + (fn == "LAG" ? -off : off);
                    if (tk >= 0 && tk < static_cast<int64_t>(m)) res = av[idx[static_cast<size_t>(tk)]];
                    else res = w.args.size() > 2 ? extra2[u] : Value();
                }
                else {
                    size_t lo = 0, hi = 0;
                    bool ok = frame(k, lo, hi);
                    if (fn == "FIRST_VALUE") res = ok ? av[idx[lo]] : Value();
                    else if (fn == "LAST_VALUE") res = ok ? av[idx[hi]] : Value();
                    else if (fn == "NTH_VALUE") {
                        int64_t nth = extra1[u].asInt();
                        if (nth < 1) throw SqlError(1210, "HY000", "Incorrect arguments to nth_value");
                        res = ok && lo + static_cast<size_t>(nth) - 1 <= hi ? av[idx[lo + static_cast<size_t>(nth) - 1]] : Value();
                    }
                    else {  // aggregate over the frame
                        if (ok && lo == plo && hi == phi) { res = prev; continue; }
                        std::vector<const Row*> g;
                        if (ok) for (size_t q = lo; q <= hi; ++q) g.push_back(&argRows[idx[q]]);
                        Value v;
                        if (g.empty()) v = (fn == "COUNT") ? Value::integer(0) : Value();
                        else v = evalAgg(aggE, Ctx{ &as1, g[0], &g, env, nullptr, nullptr });
                        res = v; prev = v;
                        if (ok) { plo = lo; phi = hi; }
                        else { plo = 1; phi = 0; }
                    }
                }
            }
        }
        return out;
    }
}  // namespace

namespace {
    std::string encodeEntries(const std::vector<LogEntry>& es) {
        std::string p;
        for (const LogEntry& e : es)
            p += "E " + std::to_string(e.time) + " " + std::to_string(e.db.size()) + " " + std::to_string(e.sql.size()) + "\n" + e.db + e.sql + "\n";
        return p;
    }
    std::vector<LogEntry> decodeEntries(const std::string& p) {
        std::vector<LogEntry> out;
        size_t pos = 0;
        while (pos < p.size()) {
            size_t nl = p.find('\n', pos);
            long long t = 0;
            size_t dl = 0, sl = 0;
            if (nl == std::string::npos || std::sscanf(p.substr(pos, nl - pos).c_str(), "E %lld %zu %zu", &t, &dl, &sl) != 3 ||
                nl + 1 + dl + sl + 1 > p.size())
                throw std::runtime_error("corrupt WAL entry");
            pos = nl + 1;
            LogEntry e;
            e.time = t;
            e.db = p.substr(pos, dl);
            e.sql = p.substr(pos + dl, sl);
            pos += dl + sl + 1;
            out.push_back(std::move(e));
        }
        return out;
    }
}  // namespace

Engine::Engine(std::string dataFile) : file_(std::move(dataFile)), wal_(file_ + ".wal") {
    env_.sub = [this](const SelectStmt& q, const void* o) { return select(q, o).rows; };
    env_.vars = &vars_;
    if (file_.empty()) return;
    cat_.load(file_);
    for (auto& dp : cat_.dbs)  // JSON-ness is not stored per value: restore it from the column type
        for (auto& tp : dp.second.tables)
            for (size_t ci = 0; ci < tp.second.cols.size(); ++ci)
                if (tp.second.cols[ci].typeName == "json")
                    for (Row& row : tp.second.rows) if (row[ci].t == Value::T::Text) row[ci].json = true;
    lsn_ = cat_.lsn;
    size_t valid = 0;
    std::vector<WalGroup> groups = wal_.readAll(valid);
    wal_.trimTo(valid);  // discard a torn tail so new groups append after valid data
    walGroups_ = groups.size();
    replaying_ = true;
    try {
        for (const WalGroup& g : groups) {
            if (g.lsn <= lsn_) continue;  // already contained in the snapshot
            for (const LogEntry& e : decodeEntries(g.payload)) {
                env_.db = e.db;
                replayTime_ = static_cast<std::time_t>(e.time);
                execute(e.sql);
            }
            lsn_ = g.lsn;
        }
    }
    catch (const SqlError& e) {
        replaying_ = false;
        throw std::runtime_error(std::string("WAL replay failed: ") + e.what());
    }
    replaying_ = false;
    env_.db.clear();
    if (walGroups_ > 0) checkpoint();
}

Engine::~Engine() {
    if (file_.empty()) return;
    try {
        if (inTxn_) cat_ = std::move(snapshot_);  // uncommitted work is discarded
        if (walGroups_ > 0) checkpoint();
    }
    catch (...) {
    }
}

std::vector<std::string> Engine::resultColumns(const std::string& sql) {
    try {
        Stmt s = parseStatement(sql);
        if (s.k != SK::Select) return {};
        s.sel->limit = 0;
        return select(*s.sel).headers;
    }
    catch (...) {
        return {};
    }
}

void Engine::resetSession() {
    if (inTxn_) { cat_ = std::move(snapshot_); snapshot_ = Catalog(); inTxn_ = false; }
    pending_.clear();
    vars_.clear();
    env_.db.clear();
    env_.lastId = 0;
}

void Engine::flushLog() {
    if (replaying_ || file_.empty() || pending_.empty()) { pending_.clear(); return; }
    wal_.append(lsn_ + 1, encodeEntries(pending_));
    ++lsn_;
    pending_.clear();
    if (++walGroups_ >= 500) checkpoint();
}

void Engine::checkpoint() {
    cat_.lsn = lsn_;
    cat_.save(file_);
    ::sync();
    wal_.reset();
    walGroups_ = 0;
}

Database& Engine::database(const std::string& name) {
    auto it = cat_.dbs.find(name);
    if (it == cat_.dbs.end()) throw SqlError(1049, "42000", "Unknown database '" + name + "'");
    return it->second;
}

Table& Engine::table(const std::string& db, const std::string& name) {
    const std::string& d = db.empty() ? env_.db : db;
    if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
    Database& dbase = database(d);
    auto it = dbase.tables.find(name);
    if (it == dbase.tables.end()) {
        if (dbase.views.count(name)) throw SqlError(1288, "HY000", "The target table " + name + " is not updatable");
        throw SqlError(1146, "42S02", "Table '" + d + "." + name + "' doesn't exist");
    }
    return it->second;
}

Table Engine::cteTable(const std::string& name, const std::vector<std::string>& cols, Result& res) {
    Table t;
    t.name = name;
    if (!cols.empty() && cols.size() != res.headers.size())
        throw SqlError(1353, "HY000", "In definition of view, derived table or common table expression, SELECT list and column names list have different column counts");
    for (size_t i = 0; i < res.headers.size(); ++i) {
        Column c;
        c.name = i < cols.size() ? cols[i] : res.headers[i];
        bool anyInt = false, anyReal = false, anyText = false;
        for (const Row& row : res.rows) {
            if (row[i].t == Value::T::Int) anyInt = true;
            else if (row[i].t == Value::T::Real) anyReal = true;
            else if (row[i].t == Value::T::Text) anyText = true;
        }
        c.type = anyText ? CType::Text : anyReal ? CType::Real : anyInt ? CType::Int : CType::Text;
        c.typeName = c.type == CType::Int ? "bigint" : c.type == CType::Real ? "double" : "text";
        t.cols.push_back(c);
    }
    t.rows = std::move(res.rows);
    return t;
}

const Table* Engine::tableFor(const TableRef& r, std::deque<Table>& temps) {
    if (!r.derived) return relation(r.db, r.name, temps);
    Result res = select(*r.derived, nullptr);
    temps.push_back(cteTable(r.alias, {}, res));
    return &temps.back();
}

const Table* Engine::relation(const std::string& db, const std::string& name, std::deque<Table>& temps) {
    if (db.empty())
        for (auto it = cteStack_.rbegin(); it != cteStack_.rend(); ++it)
            if (ciCompare(it->first, name) == 0) return it->second;
    const std::string& d = db.empty() ? env_.db : db;
    if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
    if (ciCompare(d, "information_schema") == 0) { temps.push_back(infoSchema(name)); return &temps.back(); }
    Database& dbase = database(d);
    auto it = dbase.tables.find(name);
    if (it != dbase.tables.end()) return &it->second;
    auto vt = dbase.views.find(name);
    if (vt == dbase.views.end()) throw SqlError(1146, "42S02", "Table '" + d + "." + name + "' doesn't exist");
    if (viewDepth_ >= 16) throw SqlError(1462, "HY000", "View '" + d + "." + name + "' has a recursive definition");
    std::string savedDb = env_.db;
    env_.db = d;  // views resolve their tables in their own database
    ++viewDepth_;
    auto savedCtes = std::move(cteStack_);  // a view does not see the caller's CTEs
    cteStack_.clear();
    Result res;
    try {
        Stmt vs = parseStatement(vt->second.sql);
        res = select(*vs.sel);
    }
    catch (...) {
        cteStack_ = std::move(savedCtes);
        --viewDepth_;
        env_.db = savedDb;
        throw;
    }
    cteStack_ = std::move(savedCtes);
    --viewDepth_;
    env_.db = savedDb;
    Table t;
    t.name = name;
    for (size_t i = 0; i < res.headers.size(); ++i) {
        Column c;
        c.name = i < vt->second.cols.size() ? vt->second.cols[i] : res.headers[i];
        bool anyInt = false, anyReal = false, anyText = false;
        for (const Row& row : res.rows) {
            if (row[i].t == Value::T::Int) anyInt = true;
            else if (row[i].t == Value::T::Real) anyReal = true;
            else if (row[i].t == Value::T::Text) anyText = true;
        }
        c.type = anyText ? CType::Text : anyReal ? CType::Real : anyInt ? CType::Int : CType::Text;
        c.typeName = c.type == CType::Int ? "bigint" : c.type == CType::Real ? "double" : "text";
        t.cols.push_back(c);
    }
    t.rows = std::move(res.rows);
    temps.push_back(std::move(t));
    return &temps.back();
}

Result Engine::execute(const std::string& sql) {
    Stmt s = parseStatement(sql);
    env_.now = replaying_ ? replayTime_ : std::time(nullptr);
    const std::string dbAtStart = env_.db;
    // The WAL stores statement text, so a statement that may read @variables is logged after SETs restoring them.
    std::map<std::string, Value> varsBefore;
    const bool usesVars = !replaying_ && !file_.empty() && !vars_.empty() && sql.find('@') != std::string::npos;
    if (usesVars) varsBefore = vars_;
    Result r;
    bool mutated = false;
    bool ddl = s.k == SK::CreateDb || s.k == SK::DropDb || s.k == SK::CreateTable || s.k == SK::DropTable || s.k == SK::Truncate ||
        s.k == SK::AlterTable || s.k == SK::CreateIndex || s.k == SK::DropIndex ||
        s.k == SK::CreateView || s.k == SK::DropView;
    if (ddl && inTxn_) { inTxn_ = false; flushLog(); }  // implicit commit, as in MySQL
    auto affected = [](int64_t n) { return "Query OK, " + std::to_string(n) + (n == 1 ? " row" : " rows") + " affected"; };
    switch (s.k) {
    case SK::Empty: r.silent = true; break;
    case SK::Ignored: r.msg = affected(0); break;
    case SK::CreateDb:
        if (cat_.dbs.count(s.name)) {
            if (!s.ifNotExists) throw SqlError(1007, "HY000", "Can't create database '" + s.name + "'; database exists");
            r.msg = affected(0) + ", 1 warning";
        }
        else { cat_.dbs[s.name].name = s.name; r.msg = affected(1); mutated = true; }
        break;
    case SK::DropDb: {
        auto it = cat_.dbs.find(s.name);
        if (it == cat_.dbs.end()) {
            if (!s.ifExists) throw SqlError(1008, "HY000", "Can't drop database '" + s.name + "'; database doesn't exist");
            r.msg = affected(0) + ", 1 warning";
        }
        else {
            int64_t n = static_cast<int64_t>(it->second.tables.size());
            cat_.dbs.erase(it);
            if (env_.db == s.name) env_.db.clear();
            r.msg = affected(n);
            mutated = true;
        }
        break;
    }
    case SK::Use: if (ciCompare(s.name, "information_schema") != 0) database(s.name); env_.db = s.name; r.msg = "Database changed"; r.timing = false; break;
    case SK::ShowDbs: {
        r.isSet = true; r.headers = { s.like.empty() ? "Database" : "Database (" + s.like + ")" };
        std::vector<std::string> names = { "information_schema" };
        for (const auto& d : cat_.dbs) names.push_back(d.first);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) if (s.like.empty() || likeMatch(lowerStr(n), 0, lowerStr(s.like), 0)) r.rows.push_back({ Value::text(n) });
        break;
    }
    case SK::ShowTables: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        r.isSet = true; r.headers = { "Tables_in_" + d + (s.like.empty() ? "" : " (" + s.like + ")") };
        if (s.full) r.headers.push_back("Table_type");
        std::map<std::string, bool> names;  // name -> is view
        for (const auto& t : database(d).tables) names[t.first] = false;
        for (const auto& v : database(d).views) names[v.first] = true;
        for (const auto& n : names) {
            if (!s.like.empty() && !likeMatch(lowerStr(n.first), 0, lowerStr(s.like), 0)) continue;
            Row row = { Value::text(n.first) };
            if (s.full) row.push_back(Value::text(n.second ? "VIEW" : "BASE TABLE"));
            r.rows.push_back(std::move(row));
        }
        break;
    }
    case SK::ShowVars:
        r.isSet = true;
        r.headers = { "Variable_name", "Value" };
        if (s.op != "status")
            for (const auto& kv : sysVars())
                if (s.newName.empty() || likeMatch(kv.first, 0, s.newName, 0)) r.rows.push_back({ Value::text(kv.first), Value::text(kv.second) });
        break;
    case SK::ShowWarnings: r.isSet = true; r.headers = { "Level", "Code", "Message" }; break;
    case SK::ShowColumns: r = describe(s); break;
    case SK::ShowIndex: {
        Table& t = table(s.db, s.name);
        r.isSet = true;
        r.headers = { "Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name", "Collation", "Cardinality", "Sub_part", "Packed", "Null", "Index_type", "Comment", "Index_comment", "Visible", "Expression" };
        for (size_t k = 0; k < t.nIndexes(); ++k) {
            const UniqueKey& u = t.indexDef(k);
            for (size_t j = 0; j < u.cols.size(); ++j) {
                std::set<std::string> distinct;  // distinct values of the first j+1 key columns
                for (const Row& row : t.rows) {
                    std::string key;
                    for (size_t q = 0; q <= j; ++q) { key += keyOf(row[u.cols[q]]); key += '\x1f'; }
                    distinct.insert(key);
                }
                r.rows.push_back({ Value::text(t.name), Value::integer(k < t.uniques.size() ? 0 : 1), Value::text(u.name), Value::integer(static_cast<int64_t>(j) + 1),
                                   Value::text(t.cols[u.cols[j]].name), Value::text("A"), Value::integer(static_cast<int64_t>(distinct.size())), Value(), Value(),
                                   Value::text(t.cols[u.cols[j]].notNull ? "" : "YES"), Value::text("BTREE"), Value::text(""), Value::text(""), Value::text("YES"), Value() });
            }
        }
        break;
    }
    case SK::ShowTableStatus: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        r.isSet = true;
        r.headers = { "Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length", "Data_length", "Max_data_length", "Index_length", "Data_free", "Auto_increment",
                      "Create_time", "Update_time", "Check_time", "Collation", "Checksum", "Create_options", "Comment" };
        Database& db = database(d);
        std::map<std::string, const Table*> names;
        for (const auto& t : db.tables) names[t.first] = &t.second;
        for (const auto& v : db.views) names[v.first] = nullptr;
        for (const auto& n : names) {
            if (!s.like.empty() && !likeMatch(lowerStr(n.first), 0, lowerStr(s.like), 0)) continue;
            if (!n.second) {
                r.rows.push_back({ Value::text(n.first), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value(), Value::text("VIEW") });
                continue;
            }
            const Table& t = *n.second;
            bool ai = false;
            for (const Column& c : t.cols) if (c.autoInc) ai = true;
            r.rows.push_back({ Value::text(n.first), Value::text("InnoDB"), Value::integer(10), Value::text("Dynamic"), Value::integer(static_cast<int64_t>(t.rows.size())), Value::integer(0),
                               Value::integer(16384), Value::integer(0), Value::integer(static_cast<int64_t>(t.nIndexes() > 1 ? (t.nIndexes() - 1) * 16384 : 0)), Value::integer(0),
                               ai ? Value::integer(t.autoNext) : Value(), Value(), Value(), Value(), Value::text("utf8mb4_0900_ai_ci"), Value(), Value::text(""), Value::text("") });
        }
        break;
    }
    case SK::ShowCreateDb:
        database(s.name);
        r.isSet = true;
        r.headers = { "Database", "Create Database" };
        r.rows.push_back({ Value::text(s.name), Value::text("CREATE DATABASE `" + s.name + "` /*!40100 DEFAULT CHARACTER SET utf8mb4 */") });
        break;
    case SK::ShowCreateView: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        Database& db = database(d);
        auto vt = db.views.find(s.name);
        if (vt == db.views.end()) {
            if (db.tables.count(s.name)) throw SqlError(1347, "HY000", "'" + d + "." + s.name + "' is not VIEW");
            throw SqlError(1146, "42S02", "Table '" + d + "." + s.name + "' doesn't exist");
        }
        std::string cl;
        for (size_t i = 0; i < vt->second.cols.size(); ++i) cl += (i ? ", `" : " (`") + vt->second.cols[i] + "`";
        if (!cl.empty()) cl += ")";
        r.isSet = true;
        r.headers = { "View", "Create View", "character_set_client", "collation_connection" };
        r.rows.push_back({ Value::text(s.name), Value::text("CREATE VIEW `" + s.name + "`" + cl + " AS " + vt->second.sql), Value::text("utf8mb4"), Value::text("utf8mb4_0900_ai_ci") });
        break;
    }
    case SK::ShowCreate: r = showCreate(s); break;
    case SK::CreateTable: r = createTable(s); mutated = true; break;
    case SK::DropTable: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        Database& db = database(d);
        auto it = db.tables.find(s.name);
        if (it == db.tables.end()) {
            if (!s.ifExists) throw SqlError(1051, "42S02", "Unknown table '" + d + "." + s.name + "'");
            r.msg = affected(0) + ", 1 warning";
        }
        else {
            for (const FkChild& ch : fkChildren(d, s.name))
                if (ch.t != &it->second)
                    throw SqlError(3730, "HY000", "Cannot drop table '" + s.name + "' referenced by a foreign key constraint '" + ch.t->fks[ch.fk].name + "' on table '" + ch.t->name + "'.");
            db.tables.erase(it); r.msg = affected(0); mutated = true;
        }
        break;
    }
    case SK::Truncate: {
        Table& t = table(s.db, s.name);
        for (const FkChild& ch : fkChildren(s.db.empty() ? env_.db : s.db, t.name))
            if (ch.t != &t)
                throw SqlError(1701, "42000", "Cannot truncate a table referenced in a foreign key constraint (" + bq(ch.db) + "." + bq(ch.t->name) + ", CONSTRAINT " + bq(ch.t->fks[ch.fk].name) + ")");
        t.rows.clear(); t.autoNext = 1; t.invalidate(); r.msg = affected(0); mutated = true;
        break;
    }
    case SK::AlterTable: r = alter(s); mutated = true; break;
    case SK::CreateIndex: {
        Table& t = table(s.db, s.name);
        std::vector<size_t> cols;
        for (const auto& n : s.idxCols) {
            int i = t.find(n);
            if (i < 0) throw SqlError(1072, "42000", "Key column '" + n + "' doesn't exist in table");
            cols.push_back(static_cast<size_t>(i));
        }
        if (s.unique) addUnique(t, s.newName, cols);
        else { t.keys.push_back({ s.newName, cols }); t.invalidate(); }
        r.msg = affected(0);
        r.info = "Records: 0  Duplicates: 0  Warnings: 0";
        mutated = true;
        break;
    }
    case SK::DropIndex: {
        Table& t = table(s.db, s.name);
        for (size_t i = 0; i < t.uniques.size(); ++i)
            if (t.uniques[i].name != "PRIMARY" && ciCompare(t.uniques[i].name, s.newName) == 0) { t.uniques.erase(t.uniques.begin() + static_cast<long>(i)); break; }
        for (size_t i = 0; i < t.keys.size(); ++i)
            if (ciCompare(t.keys[i].name, s.newName) == 0) { t.keys.erase(t.keys.begin() + static_cast<long>(i)); break; }
        t.invalidate();
        r.msg = affected(0);
        r.info = "Records: 0  Duplicates: 0  Warnings: 0";
        mutated = true;
        break;
    }
    case SK::Insert: r = insert(s); mutated = true; break;
    case SK::Update: r = update(s); mutated = true; break;
    case SK::Delete: r = del(s); mutated = true; break;
    case SK::Select: r = select(*s.sel); break;
    case SK::Explain: r = explain(*s.sel); break;
    case SK::CreateView: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        Database& db = database(d);
        if (db.tables.count(s.name) || (db.views.count(s.name) && !s.orReplace))
            throw SqlError(1050, "42S01", "Table '" + s.name + "' already exists");
        s.sel->limit = 0;  // validate the definition without computing rows
        Result chk = select(*s.sel);
        if (!s.idxCols.empty() && s.idxCols.size() != chk.headers.size())
            throw SqlError(1353, "HY000", "In definition of view, derived table or common table expression, SELECT list and column list have different column counts");
        db.views[s.name] = ViewDef{ s.text, s.idxCols };
        r.msg = affected(0);
        mutated = true;
        break;
    }
    case SK::DropView: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        Database& db = database(d);
        auto vit = db.views.find(s.name);
        if (vit == db.views.end()) {
            if (!s.ifExists) throw SqlError(1051, "42S02", "Unknown table '" + d + "." + s.name + "'");
            r.msg = affected(0) + ", 1 warning";
        }
        else { db.views.erase(vit); r.msg = affected(0); mutated = true; }
        break;
    }
    case SK::SetVar:
        for (const auto& st : s.sets) vars_[st.first] = eval(*st.second, Ctx{ nullptr, nullptr, nullptr, &env_, nullptr });
        r.msg = affected(0);
        break;
    case SK::Begin:
        if (inTxn_) flushLog();
        snapshot_ = cat_; inTxn_ = true; r.msg = affected(0);
        break;
    case SK::Commit:
        inTxn_ = false;
        r.msg = affected(0);
        break;
    case SK::Rollback:
        if (inTxn_) {
            cat_ = std::move(snapshot_); snapshot_ = Catalog();
            inTxn_ = false;
            pending_.clear();
            if (!env_.db.empty() && !cat_.dbs.count(env_.db)) env_.db.clear();
        }
        r.msg = affected(0);
        break;
    }
    if (mutated && !replaying_ && !file_.empty()) {
        if (usesVars)
            for (const auto& kv : varsBefore) {
                std::string lit;
                if (kv.second.isNull()) lit = "NULL";
                else if (kv.second.t == Value::T::Text) {
                    lit = "'";
                    for (char ch : kv.second.s) { if (ch == '\'') lit += "''"; else if (ch == '\\') lit += "\\\\"; else lit += ch; }
                    lit += "'";
                }
                else lit = kv.second.str();
                pending_.push_back({ static_cast<int64_t>(env_.now), dbAtStart, "SET " + kv.first + " = " + lit });
            }
        pending_.push_back({ static_cast<int64_t>(env_.now), dbAtStart, sql });
    }
    if (!inTxn_) flushLog();
    return r;
}


// ===================== foreign keys =====================
namespace {
    const char* fkAct(int a) { return a == 1 ? "CASCADE" : a == 2 ? "SET NULL" : a == 3 ? "RESTRICT" : "NO ACTION"; }
}

std::string Engine::fkClause(const std::string& db, const Table& child, const ForeignKey& fk, bool show) const {
    const char* sep = show ? "," : ", ";
    std::string o = "FOREIGN KEY (";
    for (size_t i = 0; i < fk.cols.size(); ++i) o += (i ? sep : "") + bq(child.cols[fk.cols[i]].name);
    o += ") REFERENCES " + (ciCompare(fk.refDb, db) == 0 ? "" : bq(fk.refDb) + ".") + bq(fk.refTable) + " (";
    for (size_t i = 0; i < fk.refCols.size(); ++i) o += (i ? sep : "") + bq(fk.refCols[i]);
    o += ")";
    if (fk.onDelete) o += std::string(" ON DELETE ") + fkAct(fk.onDelete);
    if (fk.onUpdate) o += std::string(" ON UPDATE ") + fkAct(fk.onUpdate);
    return o;
}

std::vector<Engine::FkChild> Engine::fkChildren(const std::string& db, const std::string& parent) {
    std::vector<FkChild> out;
    for (auto& dp : cat_.dbs)
        for (auto& tp : dp.second.tables)
            for (size_t i = 0; i < tp.second.fks.size(); ++i) {
                const ForeignKey& f = tp.second.fks[i];
                if (ciCompare(f.refDb, db) == 0 && ciCompare(f.refTable, parent) == 0) out.push_back({ &tp.second, dp.first, i });
            }
    return out;
}

void Engine::fkBackup(Table* t) {
    if (!fkUndo_.count(t)) fkUndo_[t] = t->rows;
}
void Engine::fkRestore() {
    for (auto& kv : fkUndo_) { kv.first->rows = std::move(kv.second); kv.first->invalidate(); }
    fkUndo_.clear();
}

ForeignKey Engine::makeFk(const FkDef& d, Table& child, const std::string& db, size_t ordinal) {
    ForeignKey fk;
    fk.name = d.name.empty() ? child.name + "_ibfk_" + std::to_string(ordinal) : d.name;
    for (const ForeignKey& o : child.fks)
        if (ciCompare(o.name, fk.name) == 0) throw SqlError(1826, "HY000", "Duplicate foreign key constraint name '" + fk.name + "'");
    for (const std::string& cn : d.cols) {
        int i = child.find(cn);
        if (i < 0) throw SqlError(1072, "42000", "Key column '" + cn + "' doesn't exist in table");
        fk.cols.push_back(static_cast<size_t>(i));
    }
    fk.refDb = d.refDb.empty() ? db : d.refDb;
    fk.refTable = d.refTable;
    const bool self = ciCompare(fk.refDb, db) == 0 && ciCompare(fk.refTable, child.name) == 0;
    const Table* p = nullptr;
    if (self) p = &child;
    else {
        auto di = cat_.dbs.find(fk.refDb);
        if (di != cat_.dbs.end()) { auto ti = di->second.tables.find(fk.refTable); if (ti != di->second.tables.end()) p = &ti->second; }
    }
    if (!p) throw SqlError(1824, "HY000", "Failed to open the referenced table '" + fk.refTable + "'");
    fk.refCols = d.refCols;
    if (fk.refCols.empty() && p->hasPk()) for (size_t c : p->uniques[0].cols) fk.refCols.push_back(p->cols[c].name);
    if (fk.refCols.size() != fk.cols.size() || fk.cols.empty()) throw SqlError(1215, "HY000", "Cannot add foreign key constraint");
    std::vector<size_t> pcols;
    for (const std::string& rc : fk.refCols) {
        int i = p->find(rc);
        if (i < 0) throw SqlError(1822, "HY000", "Failed to add the foreign key constraint. Missing column '" + rc + "' for constraint '" + fk.name + "' in the referenced table '" + p->name + "'");
        pcols.push_back(static_cast<size_t>(i));
    }
    for (size_t k = 0; k < pcols.size(); ++k) {
        fk.refCols[k] = p->cols[pcols[k]].name;
        if (child.cols[fk.cols[k]].type != p->cols[pcols[k]].type)
            throw SqlError(3780, "HY000", "Referencing column '" + child.cols[fk.cols[k]].name + "' and referenced column '" + p->cols[pcols[k]].name + "' in foreign key constraint '" + fk.name + "' are incompatible.");
    }
    auto prefixIdx = [](const Table& tb, const std::vector<size_t>& cols) {
        for (size_t i = 0; i < tb.nIndexes(); ++i) {
            const UniqueKey& u = tb.indexDef(i);
            if (u.cols.size() >= cols.size() && std::equal(cols.begin(), cols.end(), u.cols.begin())) return true;
        }
        return false;
        };
    if (!prefixIdx(*p, pcols))
        throw SqlError(1822, "HY000", "Failed to add the foreign key constraint. Missing index for constraint '" + fk.name + "' in the referenced table '" + p->name + "'");
    if (d.onDelete == 4 || d.onUpdate == 4) throw SqlError(1215, "HY000", "Cannot add foreign key constraint");
    fk.onDelete = d.onDelete;
    fk.onUpdate = d.onUpdate;
    if (fk.onDelete == 2 || fk.onUpdate == 2)
        for (size_t c : fk.cols)
            if (child.cols[c].notNull) throw SqlError(1830, "HY000", "Column '" + child.cols[c].name + "' cannot be NOT NULL: needed in a foreign key constraint '" + fk.name + "' SET NULL");
    if (!prefixIdx(child, fk.cols)) {  // InnoDB creates a supporting index on the child
        std::string nm = d.name.empty() ? child.cols[fk.cols[0]].name : d.name;
        for (bool clash = true; clash;) {
            clash = false;
            for (size_t i = 0; i < child.nIndexes(); ++i) if (ciCompare(child.indexDef(i).name, nm) == 0) { nm += "_2"; clash = true; break; }
        }
        child.keys.push_back({ nm, fk.cols });
        child.invalidate();
    }
    return fk;
}

namespace {
    // rows of `c` whose foreign-key columns equal `key` (uses an index when one matches exactly)
    std::vector<size_t> childRows(const Table& c, const ForeignKey& fk, const std::vector<Value>& key) {
        std::vector<size_t> out;
        c.ensureIndexes();
        for (size_t i = 0; i < c.nIndexes(); ++i) {
            const UniqueKey& u = c.indexDef(i);
            if (u.cols == fk.cols) {
                auto it = c.idx[i].find(key);
                if (it != c.idx[i].end()) out = it->second;
                return out;
            }
        }
        for (size_t r = 0; r < c.rows.size(); ++r) {
            bool eq = true;
            for (size_t k = 0; k < fk.cols.size() && eq; ++k) {
                const Value& v = c.rows[r][fk.cols[k]];
                eq = !v.isNull() && compareValues(v, key[k]) == 0;
            }
            if (eq) out.push_back(r);
        }
        return out;
    }
}

void Engine::fkCheckChild(const Table& t, const std::string& db, const Row& r) {
    for (const ForeignKey& fk : t.fks) {
        std::vector<Value> key;
        bool nul = false;
        for (size_t c : fk.cols) { if (r[c].isNull()) nul = true; key.push_back(r[c]); }
        if (nul) continue;
        const bool self = ciCompare(fk.refDb, db) == 0 && ciCompare(fk.refTable, t.name) == 0;
        const Table* p = &t;
        if (!self) {
            auto di = cat_.dbs.find(fk.refDb);
            p = nullptr;
            if (di != cat_.dbs.end()) { auto ti = di->second.tables.find(fk.refTable); if (ti != di->second.tables.end()) p = &ti->second; }
        }
        bool found = false;
        if (p) {
            std::vector<size_t> pcols;
            for (const std::string& rc : fk.refCols) pcols.push_back(static_cast<size_t>(p->find(rc)));
            p->ensureIndexes();
            bool indexed = false;
            for (size_t i = 0; i < p->nIndexes() && !indexed; ++i)
                if (p->indexDef(i).cols == pcols) { indexed = true; found = p->idx[i].count(key) > 0; }
            if (!indexed)
                for (const Row& pr : p->rows) {
                    bool eq = true;
                    for (size_t k = 0; k < pcols.size() && eq; ++k) eq = !pr[pcols[k]].isNull() && compareValues(pr[pcols[k]], key[k]) == 0;
                    if (eq) { found = true; break; }
                }
            if (!found && self) {  // a row may reference itself
                bool eq = true;
                for (size_t k = 0; k < pcols.size() && eq; ++k) eq = !r[pcols[k]].isNull() && compareValues(r[pcols[k]], key[k]) == 0;
                found = eq;
            }
        }
        if (!found)
            throw SqlError(1452, "23000", "Cannot add or update a child row: a foreign key constraint fails (" + bq(db) + "." + bq(t.name) + ", CONSTRAINT " + bq(fk.name) + " " + fkClause(db, t, fk, false) + ")");
    }
}

void Engine::fkOnParentUpdate(const std::string& db, Table& parent, const Row& oldRow, const Row& newRow, int depth) {
    if (depth > 15) throw SqlError(3008, "HY000", "Foreign key cascade delete/update exceeds max depth of 15.");
    for (const FkChild& ch : fkChildren(db, parent.name)) {
        const ForeignKey& fk = ch.t->fks[ch.fk];
        std::vector<Value> okey, nkey;
        bool nul = false, same = true;
        for (const std::string& rc : fk.refCols) {
            size_t pi = static_cast<size_t>(parent.find(rc));
            okey.push_back(oldRow[pi]); nkey.push_back(newRow[pi]);
            if (oldRow[pi].isNull()) nul = true;
            if (!sameExact(oldRow[pi], newRow[pi])) same = false;
        }
        if (nul || same) continue;
        std::vector<size_t> rows = childRows(*ch.t, fk, okey);
        if (rows.empty()) continue;
        if (ch.t == &parent || fk.onUpdate == 0 || fk.onUpdate == 3)
            throw SqlError(1451, "23000", "Cannot delete or update a parent row: a foreign key constraint fails (" + bq(ch.db) + "." + bq(ch.t->name) + ", CONSTRAINT " + bq(fk.name) + " " + fkClause(ch.db, *ch.t, fk, false) + ")");
        fkBackup(ch.t);
        std::vector<std::pair<Row, Row>> changed;
        for (size_t ri : rows) {
            Row oldC = ch.t->rows[ri], nwC = oldC;
            for (size_t k = 0; k < fk.cols.size(); ++k) nwC[fk.cols[k]] = fk.onUpdate == 1 ? nkey[k] : Value();
            ch.t->rows[ri] = nwC;
            changed.emplace_back(std::move(oldC), std::move(nwC));
        }
        ch.t->invalidate();
        for (const auto& pr : changed) fkOnParentUpdate(ch.db, *ch.t, pr.first, pr.second, depth + 1);
    }
}

void Engine::fkPlanDelete(const std::string& db, Table& t, std::set<size_t>& rows) {
    struct Item { Table* tb; std::string db; size_t row; };
    struct Hit { Table* c; std::string cdb; size_t fk; size_t row; };
    std::map<Table*, std::set<size_t>> dels;
    dels[&t] = rows;
    std::vector<Item> q;
    for (size_t r : rows) q.push_back({ &t, db, r });
    std::vector<Hit> restrictHits, nullHits;
    for (size_t qi = 0; qi < q.size(); ++qi) {
        Item it = q[qi];
        for (const FkChild& ch : fkChildren(it.db, it.tb->name)) {
            const ForeignKey& fk = ch.t->fks[ch.fk];
            std::vector<Value> key;
            bool nul = false;
            for (const std::string& rc : fk.refCols) {
                const Value& v = it.tb->rows[it.row][static_cast<size_t>(it.tb->find(rc))];
                if (v.isNull()) nul = true;
                key.push_back(v);
            }
            if (nul) continue;
            for (size_t cr : childRows(*ch.t, fk, key)) {
                if (dels[ch.t].count(cr)) continue;
                if (fk.onDelete == 1) { dels[ch.t].insert(cr); q.push_back({ ch.t, ch.db, cr }); }
                else if (fk.onDelete == 2) nullHits.push_back({ ch.t, ch.db, ch.fk, cr });
                else restrictHits.push_back({ ch.t, ch.db, ch.fk, cr });
            }
        }
    }
    for (const Hit& h : restrictHits)
        if (!dels[h.c].count(h.row)) {
            const ForeignKey& fk = h.c->fks[h.fk];
            throw SqlError(1451, "23000", "Cannot delete or update a parent row: a foreign key constraint fails (" + bq(h.cdb) + "." + bq(h.c->name) + ", CONSTRAINT " + bq(fk.name) + " " + fkClause(h.cdb, *h.c, fk, false) + ")");
        }
    for (const Hit& h : nullHits) {
        if (dels[h.c].count(h.row)) continue;
        fkBackup(h.c);
        for (size_t c : h.c->fks[h.fk].cols) h.c->rows[h.row][c] = Value();
        h.c->invalidate();
    }
    for (auto& kv : dels) {
        if (kv.first == &t || kv.second.empty()) continue;
        fkBackup(kv.first);
        std::vector<Row> keep;
        for (size_t i = 0; i < kv.first->rows.size(); ++i) if (!kv.second.count(i)) keep.push_back(std::move(kv.first->rows[i]));
        kv.first->rows = std::move(keep);
        kv.first->invalidate();
    }
    rows = dels[&t];
}



void Engine::computeGenerated(const Table& t, Row& r, size_t rowNo) {
    bool any = false;
    for (const Column& c : t.cols) if (!c.gen.empty()) any = true;
    if (!any) return;
    Scope sc = scopeFor(t, t.name);
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column& c = t.cols[i];
        if (c.gen.empty()) continue;
        auto it = checkCache_.find(c.gen);
        if (it == checkCache_.end()) it = checkCache_.emplace(c.gen, std::make_shared<Stmt>(parseStatement("SELECT " + c.gen))).first;
        Value v = coerce(c, eval(*it->second->sel->items[0].e, Ctx{ &sc, &r, nullptr, &env_, nullptr }), rowNo);
        if (v.isNull() && c.notNull) throw SqlError(1048, "23000", "Column '" + c.name + "' cannot be null");
        r[i] = v;
    }
}

void Engine::checkRow(const Table& t, const Row& r) {
    if (t.checks.empty()) return;
    Scope sc = scopeFor(t, t.name);
    for (const CheckDef& c : t.checks) {
        auto it = checkCache_.find(c.expr);
        if (it == checkCache_.end()) it = checkCache_.emplace(c.expr, std::make_shared<Stmt>(parseStatement("SELECT " + c.expr))).first;
        Value v = eval(*it->second->sel->items[0].e, Ctx{ &sc, &r, nullptr, &env_, nullptr });
        if (!v.isNull() && !v.truthy()) throw SqlError(3819, "HY000", "Check constraint '" + c.name + "' is violated.");
    }
}

Table Engine::infoSchema(const std::string& nm) {
    const std::string n = [&] { std::string u = nm; for (char& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return u; }();
    Table t;
    t.name = nm;
    auto S = [](const std::string& x) { return Value::text(x); };
    auto I = [](int64_t x) { return Value::integer(x); };
    auto cols = [&](std::initializer_list<std::pair<const char*, CType>> cs) {
        for (const auto& c : cs) { Column col; col.name = c.first; col.type = c.second; col.typeName = c.second == CType::Int ? "bigint" : "text"; t.cols.push_back(col); }
        };
    const CType TX = CType::Text, IN = CType::Int;
    auto keyKind = [](const Table& tb, size_t i) {
        std::string key;
        for (const UniqueKey& u : tb.uniques) {
            if (std::find(u.cols.begin(), u.cols.end(), i) == u.cols.end()) continue;
            if (u.name == "PRIMARY") return std::string("PRI");
            if (u.cols.size() == 1 && key.empty()) key = "UNI";
        }
        if (key.empty()) {
            for (const UniqueKey& u : tb.uniques) if (u.cols.size() > 1 && u.cols[0] == i) key = "MUL";
            for (const UniqueKey& u : tb.keys) if (u.cols[0] == i && key.empty()) key = "MUL";
        }
        return key;
        };
    if (n == "SCHEMATA") {
        cols({ { "CATALOG_NAME", TX },{ "SCHEMA_NAME", TX },{ "DEFAULT_CHARACTER_SET_NAME", TX },{ "DEFAULT_COLLATION_NAME", TX } });
        t.rows.push_back({ S("def"), S("information_schema"), S("utf8mb3"), S("utf8mb3_general_ci") });
        for (const auto& d : cat_.dbs) t.rows.push_back({ S("def"), S(d.first), S("utf8mb4"), S("utf8mb4_0900_ai_ci") });
    }
    else if (n == "TABLES") {
        cols({ { "TABLE_CATALOG", TX },{ "TABLE_SCHEMA", TX },{ "TABLE_NAME", TX },{ "TABLE_TYPE", TX },{ "ENGINE", TX },{ "TABLE_ROWS", IN },{ "AUTO_INCREMENT", IN } });
        for (const auto& d : cat_.dbs) {
            for (const auto& tp : d.second.tables) {
                bool ai = false;
                for (const Column& c : tp.second.cols) if (c.autoInc) ai = true;
                t.rows.push_back({ S("def"), S(d.first), S(tp.first), S("BASE TABLE"), S("InnoDB"), I(static_cast<int64_t>(tp.second.rows.size())), ai ? I(tp.second.autoNext) : Value() });
            }
            for (const auto& vp : d.second.views) t.rows.push_back({ S("def"), S(d.first), S(vp.first), S("VIEW"), Value(), Value(), Value() });
        }
    }
    else if (n == "COLUMNS") {
        cols({ { "TABLE_CATALOG", TX },{ "TABLE_SCHEMA", TX },{ "TABLE_NAME", TX },{ "COLUMN_NAME", TX },{ "ORDINAL_POSITION", IN },{ "COLUMN_DEFAULT", TX },
               { "IS_NULLABLE", TX },{ "DATA_TYPE", TX },{ "COLUMN_TYPE", TX },{ "COLUMN_KEY", TX },{ "EXTRA", TX } });
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables)
                for (size_t i = 0; i < tp.second.cols.size(); ++i) {
                    const Column& c = tp.second.cols[i];
                    Value def = c.hasDef ? (c.defNow ? S("CURRENT_TIMESTAMP") : (c.def.isNull() ? Value() : S(c.def.str()))) : Value();
                    std::string base = c.typeName.substr(0, c.typeName.find_first_of("( "));
                    t.rows.push_back({ S("def"), S(d.first), S(tp.first), S(c.name), I(static_cast<int64_t>(i) + 1), def, S(c.notNull ? "NO" : "YES"),
                                       S(base), S(c.typeName), S(keyKind(tp.second, i)), S(c.autoInc ? "auto_increment" : !c.gen.empty() ? (c.genStored ? "STORED GENERATED" : "VIRTUAL GENERATED") : "") });
                }
    }
    else if (n == "STATISTICS") {
        cols({ { "TABLE_CATALOG", TX },{ "TABLE_SCHEMA", TX },{ "TABLE_NAME", TX },{ "NON_UNIQUE", IN },{ "INDEX_NAME", TX },{ "SEQ_IN_INDEX", IN },{ "COLUMN_NAME", TX } });
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables)
                for (size_t k = 0; k < tp.second.nIndexes(); ++k) {
                    const UniqueKey& u = tp.second.indexDef(k);
                    for (size_t j = 0; j < u.cols.size(); ++j)
                        t.rows.push_back({ S("def"), S(d.first), S(tp.first), I(k < tp.second.uniques.size() ? 0 : 1), S(u.name), I(static_cast<int64_t>(j) + 1), S(tp.second.cols[u.cols[j]].name) });
                }
    }
    else if (n == "TABLE_CONSTRAINTS") {
        cols({ { "CONSTRAINT_CATALOG", TX },{ "CONSTRAINT_SCHEMA", TX },{ "CONSTRAINT_NAME", TX },{ "TABLE_SCHEMA", TX },{ "TABLE_NAME", TX },{ "CONSTRAINT_TYPE", TX } });
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables) {
                const Table& tb = tp.second;
                for (const UniqueKey& u : tb.uniques)
                    t.rows.push_back({ S("def"), S(d.first), S(u.name), S(d.first), S(tp.first), S(u.name == "PRIMARY" ? "PRIMARY KEY" : "UNIQUE") });
                for (const ForeignKey& f : tb.fks) t.rows.push_back({ S("def"), S(d.first), S(f.name), S(d.first), S(tp.first), S("FOREIGN KEY") });
                for (const CheckDef& c : tb.checks) t.rows.push_back({ S("def"), S(d.first), S(c.name), S(d.first), S(tp.first), S("CHECK") });
            }
    }
    else if (n == "KEY_COLUMN_USAGE") {
        cols({ { "CONSTRAINT_CATALOG", TX },{ "CONSTRAINT_SCHEMA", TX },{ "CONSTRAINT_NAME", TX },{ "TABLE_CATALOG", TX },{ "TABLE_SCHEMA", TX },{ "TABLE_NAME", TX },
               { "COLUMN_NAME", TX },{ "ORDINAL_POSITION", IN },{ "REFERENCED_TABLE_SCHEMA", TX },{ "REFERENCED_TABLE_NAME", TX },{ "REFERENCED_COLUMN_NAME", TX } });
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables) {
                const Table& tb = tp.second;
                for (const UniqueKey& u : tb.uniques)
                    for (size_t j = 0; j < u.cols.size(); ++j)
                        t.rows.push_back({ S("def"), S(d.first), S(u.name), S("def"), S(d.first), S(tp.first), S(tb.cols[u.cols[j]].name), I(static_cast<int64_t>(j) + 1), Value(), Value(), Value() });
                for (const ForeignKey& f : tb.fks)
                    for (size_t j = 0; j < f.cols.size(); ++j)
                        t.rows.push_back({ S("def"), S(d.first), S(f.name), S("def"), S(d.first), S(tp.first), S(tb.cols[f.cols[j]].name), I(static_cast<int64_t>(j) + 1), S(f.refDb), S(f.refTable), S(f.refCols[j]) });
            }
    }
    else if (n == "REFERENTIAL_CONSTRAINTS") {
        cols({ { "CONSTRAINT_CATALOG", TX },{ "CONSTRAINT_SCHEMA", TX },{ "CONSTRAINT_NAME", TX },{ "UNIQUE_CONSTRAINT_SCHEMA", TX },{ "UPDATE_RULE", TX },{ "DELETE_RULE", TX },
               { "TABLE_NAME", TX },{ "REFERENCED_TABLE_NAME", TX } });
        auto rule = [](int a) { return std::string(a == 1 ? "CASCADE" : a == 2 ? "SET NULL" : a == 3 ? "RESTRICT" : "NO ACTION"); };
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables)
                for (const ForeignKey& f : tp.second.fks)
                    t.rows.push_back({ S("def"), S(d.first), S(f.name), S(f.refDb), S(rule(f.onUpdate)), S(rule(f.onDelete)), S(tp.first), S(f.refTable) });
    }
    else if (n == "CHECK_CONSTRAINTS") {
        cols({ { "CONSTRAINT_CATALOG", TX },{ "CONSTRAINT_SCHEMA", TX },{ "CONSTRAINT_NAME", TX },{ "CHECK_CLAUSE", TX } });
        for (const auto& d : cat_.dbs)
            for (const auto& tp : d.second.tables)
                for (const CheckDef& c : tp.second.checks) t.rows.push_back({ S("def"), S(d.first), S(c.name), S("(" + c.expr + ")") });
    }
    else throw SqlError(1109, "42S02", "Unknown table '" + nm + "' in information_schema");
    return t;
}

Result Engine::createTable(Stmt& s) {
    const std::string& d = s.db.empty() ? env_.db : s.db;
    if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
    Database& db = database(d);
    Result r;
    if (db.tables.count(s.name)) {
        if (!s.ifNotExists) throw SqlError(1050, "42S01", "Table '" + s.name + "' already exists");
        r.msg = "Query OK, 0 rows affected, 1 warning";
        return r;
    }
    Table t;
    t.name = s.name;
    std::vector<size_t> pk;
    for (ColDef& cd : s.cols) {
        if (t.find(cd.name) >= 0) throw SqlError(1060, "42S21", "Duplicate column name '" + cd.name + "'");
        Column c;
        c.name = cd.name; c.typeName = cd.typeName; c.type = cd.type; c.len = cd.len; c.notNull = cd.notNull;
        c.autoInc = cd.autoInc; c.hasDef = cd.hasDef; c.defNow = cd.defNow; c.def = cd.def; c.gen = cd.gen; c.genStored = cd.genStored;
        t.cols.push_back(c);
        if (cd.primary) pk.push_back(t.cols.size() - 1);
    }
    auto idxOf = [&](const std::string& n) {
        int i = t.find(n);
        if (i < 0) throw SqlError(1072, "42000", "Key column '" + n + "' doesn't exist in table");
        return static_cast<size_t>(i);
        };
    if (!s.pkCols.empty()) {
        if (!pk.empty()) throw SqlError(1068, "42000", "Multiple primary key defined");
        for (const auto& n : s.pkCols) pk.push_back(idxOf(n));
    }
    if (!pk.empty()) {
        for (size_t i : pk) t.cols[i].notNull = true;
        t.uniques.push_back({ "PRIMARY", pk });
    }
    for (size_t i = 0; i < s.cols.size(); ++i) if (s.cols[i].unique) t.uniques.push_back({ s.cols[i].name,{ i } });
    for (const auto& kd : s.keys) {
        UniqueKey k;
        k.name = kd.first;
        for (const auto& n : kd.second) k.cols.push_back(idxOf(n));
        t.keys.push_back(k);
    }
    for (size_t ui = 0; ui < s.uniqueKeys.size(); ++ui) {
        const auto& uk = s.uniqueKeys[ui];
        UniqueKey u;
        u.name = ui < s.uniqueNames.size() && !s.uniqueNames[ui].empty() ? s.uniqueNames[ui] : uk[0];
        for (const auto& n : uk) u.cols.push_back(idxOf(n));
        t.uniques.push_back(u);
    }
    for (const FkDef& fd : s.fks) t.fks.push_back(makeFk(fd, t, d, t.fks.size() + 1));
    {
        std::vector<std::pair<std::string, std::string>> all;
        for (const ColDef& cd : s.cols) if (!cd.check.empty()) all.emplace_back("", cd.check);
        for (const auto& c : s.checks) all.push_back(c);
        for (const auto& c : all) {
            std::string nm = c.first.empty() ? t.name + "_chk_" + std::to_string(t.checks.size() + 1) : c.first;
            for (const CheckDef& o : t.checks) if (ciCompare(o.name, nm) == 0) throw SqlError(3822, "HY000", "Duplicate check constraint name '" + nm + "'.");
            t.checks.push_back({ nm, c.second });
            try { checkRow(t, Row(t.cols.size())); }  // validates the expression (unknown columns, bad syntax)
            catch (const SqlError& e) { if (e.code != 3819) throw; }
        }
    }
    db.tables[t.name] = std::move(t);
    r.msg = "Query OK, 0 rows affected";
    return r;
}

Result Engine::insert(Stmt& s) {
    Table& t = table(s.db, s.name);
    std::vector<size_t> colIdx;
    if (s.insCols.empty()) for (size_t i = 0; i < t.cols.size(); ++i) colIdx.push_back(i);
    else for (const auto& n : s.insCols) {
        int i = t.find(n);
        if (i < 0) throw SqlError(1054, "42S22", "Unknown column '" + n + "' in 'field list'");
        colIdx.push_back(static_cast<size_t>(i));
    }
    size_t orig = t.rows.size();
    bool idSet = false;
    int64_t firstId = 0;
    t.ensureIndexes();
    Scope sc = scopeFor(t, t.name);
    const std::string dbn = s.db.empty() ? env_.db : s.db;
    const bool fkChild = !t.fks.empty(), fkParent = (s.replace || !s.onDup.empty()) && !fkChildren(dbn, t.name).empty();
    fkUndo_.clear();
    int64_t affected = 0, dups = 0, warns = 0, records = 0;
    // child-side check; with INSERT IGNORE a violating row is skipped with a warning
    auto fkOk = [&](const Row& row) {
        if (!fkChild && t.checks.empty()) return true;
        try { checkRow(t, row); if (fkChild) fkCheckChild(t, dbn, row); }
        catch (const SqlError& e) { if (s.ignore && (e.code == 1452 || e.code == 3819)) { ++dups; ++warns; return false; } throw; }
        return true;
        };
    const bool rewrites = s.replace || !s.onDup.empty();
    std::vector<Row> backup;
    const int64_t autoBefore = t.autoNext;
    if (rewrites) backup = t.rows;
    auto addRow = [&](const std::vector<Value>& vals, const std::vector<bool>& dflt, size_t rowNo) {
        ++records;
        if (vals.size() != colIdx.size())
            throw SqlError(1136, "21S01", "Column count doesn't match value count at row " + std::to_string(rowNo));
        Row r(t.cols.size());
        std::vector<bool> given(t.cols.size(), false);
        for (size_t k = 0; k < vals.size(); ++k) if (!dflt[k]) { r[colIdx[k]] = vals[k]; given[colIdx[k]] = true; }
        for (size_t ci = 0; ci < t.cols.size(); ++ci) {
            const Column& c = t.cols[ci];
            if (!c.gen.empty()) {
                if (given[ci]) throw SqlError(3105, "HY000", "The value specified for generated column '" + c.name + "' in table '" + t.name + "' is not allowed.");
                continue;
            }
            if (!given[ci]) {
                if (c.autoInc) r[ci] = Value();
                else if (c.hasDef) r[ci] = defaultFor(c, env_.now);
                else if (c.notNull) throw SqlError(1364, "HY000", "Field '" + c.name + "' doesn't have a default value");
            }
            r[ci] = coerce(c, r[ci], rowNo);
            if (c.autoInc) {
                if (r[ci].isNull() || r[ci].asInt() == 0) {
                    r[ci] = coerce(c, Value::integer(t.autoNext++), rowNo);
                    if (!idSet) { idSet = true; firstId = r[ci].i; }
                }
                else if (r[ci].asInt() >= t.autoNext) t.autoNext = r[ci].asInt() + 1;
            }
            else if (r[ci].isNull() && c.notNull) throw SqlError(1048, "23000", "Column '" + c.name + "' cannot be null");
        }
        computeGenerated(t, r, rowNo);
        std::vector<size_t> conflicts;
        size_t firstU = 0;
        for (size_t u = 0; u < t.uniques.size(); ++u) {
            std::vector<Value> k;
            if (!t.makeKey(t.uniques[u], r, k)) continue;
            auto it = t.idx[u].find(k);
            if (it == t.idx[u].end()) continue;
            if (conflicts.empty()) firstU = u;
            for (size_t o : it->second) if (std::find(conflicts.begin(), conflicts.end(), o) == conflicts.end()) conflicts.push_back(o);
        }
        if (!conflicts.empty()) {
            if (s.ignore) { ++dups; ++warns; return; }
            if (!s.onDup.empty()) {
                size_t ri = conflicts[0];
                Row old = t.rows[ri], nw = old;
                env_.insTable = &t; env_.insRow = &r;
                Ctx c{ &sc, &nw, nullptr, &env_, nullptr };
                try {
                    for (const auto& st : s.onDup) {
                        int i = t.find(st.first);
                        if (i < 0) throw SqlError(1054, "42S22", "Unknown column '" + st.first + "' in 'field list'");
                        const Column& col = t.cols[static_cast<size_t>(i)];
                        if (!col.gen.empty()) throw SqlError(3105, "HY000", "The value specified for generated column '" + col.name + "' in table '" + t.name + "' is not allowed.");
                        Value v = coerce(col, eval(*st.second, c), rowNo);
                        if (v.isNull() && col.notNull) throw SqlError(1048, "23000", "Column '" + col.name + "' cannot be null");
                        nw[static_cast<size_t>(i)] = v;
                    }
                    computeGenerated(t, nw, rowNo);
                }
                catch (...) { env_.insTable = nullptr; env_.insRow = nullptr; throw; }
                env_.insTable = nullptr; env_.insRow = nullptr;
                bool same = true;
                for (size_t k = 0; k < nw.size(); ++k) if (!sameExact(nw[k], old[k])) same = false;
                ++dups;
                if (same) return;
                for (size_t u = 0; u < t.uniques.size(); ++u) {
                    std::vector<Value> k;
                    if (!t.makeKey(t.uniques[u], nw, k)) continue;
                    auto it = t.idx[u].find(k);
                    if (it != t.idx[u].end()) for (size_t other : it->second) if (other != ri) throw dupError(t.uniques[u], nw);
                }
                if (!fkOk(nw)) return;
                if (fkParent) fkOnParentUpdate(dbn, t, old, nw, 0);
                t.indexRemove(old, ri);
                t.rows[ri] = nw;
                t.indexAdd(nw, ri);
                affected += 2;
                return;
            }
            if (!s.replace) throw dupError(t.uniques[firstU], r);
            // REPLACE: the new row takes the slot of the first conflicting row; any further conflicts are deleted.
            std::sort(conflicts.begin(), conflicts.end());
            size_t ri = conflicts[0];
            if (!fkOk(r)) return;
            if (fkParent) fkOnParentUpdate(dbn, t, t.rows[ri], r, 0);
            t.indexRemove(t.rows[ri], ri);
            t.rows[ri] = r;
            t.indexAdd(t.rows[ri], ri);
            affected += 1 + static_cast<int64_t>(conflicts.size());
            if (conflicts.size() > 1) {
                std::vector<bool> kill(t.rows.size(), false);
                for (size_t q = 1; q < conflicts.size(); ++q) kill[conflicts[q]] = true;
                std::vector<Row> keep;
                for (size_t q = 0; q < t.rows.size(); ++q) if (!kill[q]) keep.push_back(std::move(t.rows[q]));
                t.rows = std::move(keep);
                t.invalidate();
                t.ensureIndexes();
            }
            return;
        }
        if (!fkOk(r)) return;
        t.rows.push_back(std::move(r));
        t.indexAdd(t.rows.back(), t.rows.size() - 1);
        ++affected;
        };
    try {
        if (s.sel) {
            Result sr = select(*s.sel);
            size_t n = 0;
            for (const Row& row : sr.rows) addRow(row, std::vector<bool>(row.size(), false), ++n);
        }
        else {
            size_t n = 0;
            for (const auto& row : s.rows) {
                std::vector<Value> vals;
                std::vector<bool> dflt;
                for (const auto& e : row) {
                    bool isDef = e->k == EK::Func && e->name == "DEFAULT";
                    dflt.push_back(isDef);
                    vals.push_back(isDef ? Value() : eval(*e, Ctx{ nullptr, nullptr, nullptr, &env_, nullptr }));
                }
                addRow(vals, dflt, ++n);
            }
        }
    }
    catch (...) {
        if (rewrites) { t.rows = std::move(backup); t.autoNext = autoBefore; }
        else t.rows.resize(orig);
        t.invalidate();
        fkRestore();
        throw;
    }
    fkUndo_.clear();
    if (idSet) env_.lastId = firstId;
    int64_t n = affected;
    Result r;
    r.msg = "Query OK, " + std::to_string(n) + (n == 1 ? " row" : " rows") + " affected";
    if (warns) r.msg += ", " + std::to_string(warns) + (warns == 1 ? " warning" : " warnings");
    if (records != 1) r.info = "Records: " + std::to_string(records) + "  Duplicates: " + std::to_string(dups) + "  Warnings: " + std::to_string(warns);
    return r;
}

Result Engine::update(Stmt& s) {
    Table& t = table(s.db, s.name);
    Scope sc = scopeFor(t, t.name);
    std::vector<size_t> setIdx;
    for (const auto& st : s.sets) {
        int i = t.find(st.first);
        if (i < 0) throw SqlError(1054, "42S22", "Unknown column '" + st.first + "' in 'field list'");
        setIdx.push_back(static_cast<size_t>(i));
    }
    std::vector<size_t> match = matchRows(t, sc, s.where.get(), s.order, s.limit, &env_);
    std::vector<Row> backup = t.rows;
    t.ensureIndexes();
    int64_t changed = 0;
    const std::string dbn = s.db.empty() ? env_.db : s.db;
    const bool fkChild = !t.fks.empty(), fkParent = !fkChildren(dbn, t.name).empty();
    fkUndo_.clear();
    try {
        for (size_t ri : match) {
            Row old = t.rows[ri], nw = old;
            Ctx c{ &sc, &old, nullptr, &env_, nullptr };
            for (size_t k = 0; k < setIdx.size(); ++k) {
                const Column& col = t.cols[setIdx[k]];
                if (!col.gen.empty()) throw SqlError(3105, "HY000", "The value specified for generated column '" + col.name + "' in table '" + t.name + "' is not allowed.");
                Value v = coerce(col, eval(*s.sets[k].second, c), ri + 1);
                if (v.isNull() && col.notNull) throw SqlError(1048, "23000", "Column '" + col.name + "' cannot be null");
                nw[setIdx[k]] = v;
            }
            computeGenerated(t, nw, ri + 1);
            bool same = true;
            for (size_t k = 0; k < nw.size(); ++k) if (!sameExact(nw[k], old[k])) same = false;
            if (!same) {
                for (size_t u = 0; u < t.uniques.size(); ++u) {
                    std::vector<Value> k;
                    if (!t.makeKey(t.uniques[u], nw, k)) continue;
                    auto it = t.idx[u].find(k);
                    if (it != t.idx[u].end()) for (size_t other : it->second) if (other != ri) throw dupError(t.uniques[u], nw);
                }
                checkRow(t, nw);
                if (fkChild) fkCheckChild(t, dbn, nw);
                if (fkParent) fkOnParentUpdate(dbn, t, old, nw, 0);
                t.indexRemove(old, ri);
                t.rows[ri] = nw;
                t.indexAdd(nw, ri);
                ++changed;
            }
        }
    }
    catch (...) {
        t.rows = std::move(backup);
        t.invalidate();
        fkRestore();
        throw;
    }
    fkUndo_.clear();
    Result r;
    r.msg = "Query OK, " + std::to_string(changed) + (changed == 1 ? " row" : " rows") + " affected";
    r.info = "Rows matched: " + std::to_string(match.size()) + "  Changed: " + std::to_string(changed) + "  Warnings: 0";
    return r;
}

Result Engine::del(Stmt& s) {
    Table& t = table(s.db, s.name);
    Scope sc = scopeFor(t, t.name);
    std::vector<size_t> match = matchRows(t, sc, s.where.get(), s.order, s.limit, &env_);
    std::vector<bool> kill(t.rows.size(), false);
    for (size_t i : match) kill[i] = true;
    {
        const std::string dbn = s.db.empty() ? env_.db : s.db;
        if (!match.empty() && !fkChildren(dbn, t.name).empty()) {  // RESTRICT / CASCADE / SET NULL on referencing rows
            std::set<size_t> M(match.begin(), match.end());
            fkUndo_.clear();
            try { fkPlanDelete(dbn, t, M); }
            catch (...) { fkRestore(); throw; }
            for (size_t i : M) kill[i] = true;
        }
    }
    std::vector<Row> keep;
    for (size_t i = 0; i < t.rows.size(); ++i) if (!kill[i]) keep.push_back(std::move(t.rows[i]));
    t.rows = std::move(keep);
    t.invalidate();
    fkUndo_.clear();
    Result r;
    r.msg = "Query OK, " + std::to_string(match.size()) + (match.size() == 1 ? " row" : " rows") + " affected";
    return r;
}

Result Engine::describe(const Stmt& s) {
    std::deque<Table> temps;
    const Table& t = *relation(s.db, s.name, temps);
    Result r;
    r.isSet = true;
    r.headers = { "Field", "Type", "Null", "Key", "Default", "Extra" };
    if (s.full) r.headers = { "Field", "Type", "Collation", "Null", "Key", "Default", "Extra", "Privileges", "Comment" };
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column& c = t.cols[i];
        if (!s.like.empty() && !likeMatch(lowerStr(c.name), 0, lowerStr(s.like), 0)) continue;
        std::string key;
        for (const UniqueKey& u : t.uniques) {
            bool in = std::find(u.cols.begin(), u.cols.end(), i) != u.cols.end();
            if (!in) continue;
            if (u.name == "PRIMARY") { key = "PRI"; break; }
            if (u.cols.size() == 1 && key.empty()) key = "UNI";
        }
        if (key.empty()) {
            for (const UniqueKey& u : t.uniques) if (u.cols.size() > 1 && u.cols[0] == i) key = "MUL";
            for (const UniqueKey& u : t.keys) if (u.cols[0] == i && key.empty()) key = "MUL";
        }
        Value def = c.hasDef ? (c.defNow ? Value::text("CURRENT_TIMESTAMP") : (c.def.t == Value::T::Null ? Value() : Value::text(c.def.str()))) : Value();
        if (s.full)
            r.rows.push_back({ Value::text(c.name), Value::text(c.typeName), c.type == CType::Text ? Value::text("utf8mb4_0900_ai_ci") : Value(), Value::text(c.notNull ? "NO" : "YES"),
                              Value::text(key), def, Value::text(c.autoInc ? "auto_increment" : !c.gen.empty() ? (c.genStored ? "STORED GENERATED" : "VIRTUAL GENERATED") : ""), Value::text("select,insert,update,references"), Value::text("") });
        else
            r.rows.push_back({ Value::text(c.name), Value::text(c.typeName), Value::text(c.notNull ? "NO" : "YES"),
                              Value::text(key), def, Value::text(c.autoInc ? "auto_increment" : !c.gen.empty() ? (c.genStored ? "STORED GENERATED" : "VIRTUAL GENERATED") : "") });
    }
    return r;
}

Result Engine::showCreate(const Stmt& s) {
    Table& t = table(s.db, s.name);
    std::string o = "CREATE TABLE `" + t.name + "` (\n";
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column& c = t.cols[i];
        o += "  `" + c.name + "` " + c.typeName;
        if (!c.gen.empty()) o += " GENERATED ALWAYS AS (" + c.gen + ") " + (c.genStored ? "STORED" : "VIRTUAL");
        if (c.notNull) o += " NOT NULL";
        if (c.autoInc) o += " AUTO_INCREMENT";
        if (c.hasDef && c.defNow) o += " DEFAULT CURRENT_TIMESTAMP";
        else if (c.hasDef && !c.def.isNull()) o += " DEFAULT " + quoteLit(c.def.str());
        else if (!c.notNull && c.gen.empty()) o += " DEFAULT NULL";
        o += (i + 1 < t.cols.size() || t.nIndexes() > 0 || !t.fks.empty() || !t.checks.empty()) ? ",\n" : "\n";
    }
    for (size_t u = 0; u < t.nIndexes(); ++u) {
        const UniqueKey& k = t.indexDef(u);
        o += k.name == "PRIMARY" ? "  PRIMARY KEY (" : (u < t.uniques.size() ? "  UNIQUE KEY `" : "  KEY `") + k.name + "` (";
        for (size_t j = 0; j < k.cols.size(); ++j) o += (j ? ",`" : "`") + t.cols[k.cols[j]].name + "`";
        o += (u + 1 < t.nIndexes() || !t.fks.empty() || !t.checks.empty()) ? "),\n" : ")\n";
    }
    {
        const std::string sdb = s.db.empty() ? env_.db : s.db;
        for (size_t f = 0; f < t.fks.size(); ++f)
            o += "  CONSTRAINT " + bq(t.fks[f].name) + " " + fkClause(sdb, t, t.fks[f], true) + (f + 1 < t.fks.size() || !t.checks.empty() ? ",\n" : "\n");
        for (size_t c = 0; c < t.checks.size(); ++c)
            o += "  CONSTRAINT " + bq(t.checks[c].name) + " CHECK (" + t.checks[c].expr + ")" + (c + 1 < t.checks.size() ? ",\n" : "\n");
    }
    o += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    Result r;
    r.isSet = true;
    r.headers = { "Table", "Create Table" };
    r.rows.push_back({ Value::text(t.name), Value::text(o) });
    return r;
}

Result Engine::selectOne(const SelectStmt& s, const void* outer) {
    const Ctx* outerCtx = static_cast<const Ctx*>(outer);
    Scope sc;
    std::vector<Row> rows;
    std::deque<Table> temps;  // materialized views for this query
    if (!s.hasFrom) rows.push_back(Row{});
    else {
        const Table& t0 = *tableFor(s.from, temps);
        sc = scopeFor(t0, s.from.alias.empty() ? t0.name : s.from.alias);
        IndexPlan ip = planIndex(t0, sc, s.where.get(), !s.joins.empty(), &env_, outerCtx, s.joins.empty() && outerCtx != nullptr);
        if (ip.used) { rows.reserve(ip.rows.size()); for (size_t id : ip.rows) rows.push_back(t0.rows[id]); }
        else rows = t0.rows;
        for (const JoinClause& j : s.joins) {
            const Table& tr = *tableFor(j.ref, temps);
            Scope rs = scopeFor(tr, j.ref.alias.empty() ? tr.name : j.ref.alias), ns = sc;
            ns.cols.insert(ns.cols.end(), rs.cols.begin(), rs.cols.end());
            size_t lw = sc.cols.size(), rw = tr.cols.size();
            std::vector<Row> out;
            std::vector<bool> rmatched(tr.rows.size(), false);
            int lk = -1, rk = -1;
            if (j.on) hashKeys(*j.on, sc, rs, lk, rk);
            int ji = -1;  // right-table index usable for probing (leading column = join column)
            if (lk >= 0) {
                tr.ensureIndexes();
                for (size_t i = 0; i < tr.nIndexes(); ++i)
                    if (tr.indexDef(i).cols[0] == static_cast<size_t>(rk) && (ji < 0 || tr.indexDef(i).cols.size() == 1)) ji = static_cast<int>(i);
            }
            std::unordered_map<std::string, std::vector<size_t>> hidx;
            if (lk >= 0 && ji < 0)
                for (size_t ri = 0; ri < tr.rows.size(); ++ri)
                    if (!tr.rows[ri][static_cast<size_t>(rk)].isNull()) hidx[keyOf(tr.rows[ri][static_cast<size_t>(rk)])].push_back(ri);
            for (const Row& l : rows) {
                bool any = false;
                auto tryRight = [&](size_t ri) {
                    Row cr = l;
                    cr.insert(cr.end(), tr.rows[ri].begin(), tr.rows[ri].end());
                    if (j.on && !eval(*j.on, Ctx{ &ns, &cr, nullptr, &env_, nullptr, outerCtx }).truthy()) return;
                    any = true;
                    rmatched[ri] = true;
                    out.push_back(std::move(cr));
                    };
                if (ji >= 0) {
                    const Value& kv = l[static_cast<size_t>(lk)];
                    if (!kv.isNull()) {
                        std::vector<size_t> ids;
                        const IndexMap& im = tr.idx[static_cast<size_t>(ji)];
                        for (auto it = im.lower_bound(std::vector<Value>{kv}); it != im.end() && compareValues(it->first[0], kv) == 0; ++it)
                            ids.insert(ids.end(), it->second.begin(), it->second.end());
                        std::sort(ids.begin(), ids.end());
                        for (size_t ri : ids) tryRight(ri);
                    }
                }
                else if (lk >= 0) {
                    const Value& kv = l[static_cast<size_t>(lk)];
                    if (!kv.isNull()) {
                        auto it = hidx.find(keyOf(kv));
                        if (it != hidx.end()) for (size_t ri : it->second) tryRight(ri);
                    }
                }
                else {
                    for (size_t ri = 0; ri < tr.rows.size(); ++ri) tryRight(ri);
                }
                if (!any && j.type == JoinClause::Type::Left) { Row cr = l; cr.resize(lw + rw); out.push_back(std::move(cr)); }
            }
            if (j.type == JoinClause::Type::Right)
                for (size_t ri = 0; ri < tr.rows.size(); ++ri)
                    if (!rmatched[ri]) { Row cr(lw); cr.insert(cr.end(), tr.rows[ri].begin(), tr.rows[ri].end()); out.push_back(std::move(cr)); }
            rows = std::move(out);
            sc = std::move(ns);
        }
    }
    std::vector<const Row*> src;
    for (const Row& r : rows)
        if (!s.where || eval(*s.where, Ctx{ &sc, &r, nullptr, &env_, nullptr, outerCtx }).truthy()) src.push_back(&r);

    struct Out { const Expr* e; size_t idx; std::string header; };
    std::vector<Out> outs;
    for (const SelItem& it : s.items) {
        if (it.star) {
            bool any = false;
            for (size_t i = 0; i < sc.cols.size(); ++i)
                if (it.starTable.empty() || sc.cols[i].table == it.starTable) { outs.push_back({ nullptr, i, sc.cols[i].name }); any = true; }
            if (!any) throw SqlError(1146, "42S02", "Unknown table '" + it.starTable + "'");
        }
        else {
            outs.push_back({ it.e.get(), 0, !it.alias.empty() ? it.alias : (it.e->k == EK::Col ? it.e->name : it.text) });
        }
    }
    bool agg = !s.groupBy.empty();
    for (const SelItem& it : s.items) if (it.e && hasAgg(*it.e)) agg = true;
    if (s.having && hasAgg(*s.having)) agg = true;
    for (const auto& o : s.order) if (hasAgg(*o.e)) agg = true;

    struct Ent { std::vector<Value> out, keys; };
    std::vector<Ent> ents;
    // ORDER BY resolution: ordinal / alias -> output index; otherwise expression.
    std::vector<int> okIdx;
    for (const auto& o : s.order) {
        int oi = -1;
        if (o.e->k == EK::Lit && o.e->v.t == Value::T::Int) {
            if (o.e->v.i < 1 || o.e->v.i > static_cast<int64_t>(outs.size()))
                throw SqlError(1054, "42S22", "Unknown column '" + std::to_string(o.e->v.i) + "' in 'order clause'");
            oi = static_cast<int>(o.e->v.i - 1);
        }
        else if (o.e->k == EK::Col && o.e->table.empty()) {
            size_t cnt = 0;
            for (const SelItem& it : s.items) {
                if (it.star) {
                    for (size_t i = 0; i < sc.cols.size(); ++i)
                        if (it.starTable.empty() || sc.cols[i].table == it.starTable) ++cnt;
                    continue;
                }
                if (!it.alias.empty() && ciCompare(it.alias, o.e->name) == 0) { oi = static_cast<int>(cnt); break; }
                ++cnt;
            }
        }
        okIdx.push_back(oi);
    }
    std::vector<Unit> units;
    auto emit = [&](const Row* row, const std::vector<const Row*>* group) {
        Ent en;
        Ctx c{ &sc, row, group, &env_, nullptr, outerCtx };
        for (const Out& o : outs) en.out.push_back(o.e ? eval(*o.e, c) : (row ? (*row)[o.idx] : Value()));
        Ctx oc{ &sc, row, group, &env_, &s.items, outerCtx };
        for (size_t i = 0; i < s.order.size(); ++i)
            en.keys.push_back(okIdx[i] >= 0 ? en.out[static_cast<size_t>(okIdx[i])] : eval(*s.order[i].e, oc));
        ents.push_back(std::move(en));
        };
    std::vector<std::vector<const Row*>> groups;  // must outlive `units` (which point into it)
    if (!agg) {
        for (const Row* r : src) {
            if (s.having && !eval(*s.having, Ctx{ &sc, r, nullptr, &env_, &s.items, outerCtx }).truthy()) continue;
            units.push_back({ r, nullptr });
        }
    }
    else {
        std::vector<const Expr*> gex;
        for (const auto& g : s.groupBy) {
            const Expr* ge = g.get();
            if (ge->k == EK::Lit && ge->v.t == Value::T::Int && ge->v.i >= 1 && ge->v.i <= static_cast<int64_t>(s.items.size()) &&
                s.items[static_cast<size_t>(ge->v.i - 1)].e)
                ge = s.items[static_cast<size_t>(ge->v.i - 1)].e.get();
            else if (ge->k == EK::Col && ge->table.empty()) {
                try { resolveCol(*ge, sc); }
                catch (const SqlError&) {
                    for (const SelItem& it : s.items) if (it.e && !it.alias.empty() && ciCompare(it.alias, ge->name) == 0) { ge = it.e.get(); break; }
                }
            }
            gex.push_back(ge);
        }
        if (gex.empty()) { groups.emplace_back(src); }
        else {
            std::map<std::string, size_t> gi;
            for (const Row* r : src) {
                std::string key;
                for (const Expr* g : gex) { key += keyOf(eval(*g, Ctx{ &sc, r, nullptr, &env_, nullptr, outerCtx })); key += '\x1f'; }
                auto it = gi.find(key);
                if (it == gi.end()) { it = gi.emplace(key, groups.size()).first; groups.emplace_back(); }
                groups[it->second].push_back(r);
            }
        }
        for (const auto& g : groups) {
            const Row* first = g.empty() ? nullptr : g[0];
            if (s.having && !eval(*s.having, Ctx{ &sc, first, &g, &env_, &s.items, outerCtx }).truthy()) continue;
            units.push_back({ first, &g });
        }
    }
    {
        std::vector<const Expr*> wins;
        for (const SelItem& it : s.items) if (it.e) collectWin(*it.e, wins);
        for (const auto& o : s.order) collectWin(*o.e, wins);
        std::vector<std::vector<Value>> wv;
        for (const Expr* w : wins) wv.push_back(computeWindow(*w, sc, units, &env_, outerCtx));
        std::vector<std::pair<const Expr*, Value>> cur(wins.size());
        struct WinGuard {
            const Env& e; const std::vector<std::pair<const Expr*, Value>>* saved;
            ~WinGuard() { e.curWin = saved; }
        } guard{ env_, env_.curWin };
        for (size_t i = 0; i < units.size(); ++i) {
            for (size_t j = 0; j < wins.size(); ++j) cur[j] = { wins[j], wv[j][i] };
            env_.curWin = wins.empty() ? nullptr : &cur;
            emit(units[i].row, units[i].group);
        }
    }
    if (!s.order.empty())
        std::stable_sort(ents.begin(), ents.end(), [&](const Ent& a, const Ent& b) { return keysLess(a.keys, b.keys, s.order); });
    Result r;
    r.isSet = true;
    for (const Out& o : outs) r.headers.push_back(o.header);
    std::set<std::string> seen;
    int64_t skip = s.offset;
    for (Ent& en : ents) {
        if (s.distinct) {
            std::string k;
            for (const Value& v : en.out) { k += keyOf(v); k += '\x1f'; }
            if (!seen.insert(k).second) continue;
        }
        if (skip > 0) { --skip; continue; }
        if (s.limit >= 0 && static_cast<int64_t>(r.rows.size()) >= s.limit) break;
        r.rows.push_back(std::move(en.out));
    }
    return r;
}

namespace {
    bool refersTo(const SelectStmt& m, const std::string& name) {
        auto hit = [&](const TableRef& t) { return !t.derived && t.db.empty() && ciCompare(t.name, name) == 0; };
        if (m.hasFrom && hit(m.from)) return true;
        for (const JoinClause& j : m.joins) if (hit(j.ref)) return true;
        return false;
    }
}

Result Engine::select(const SelectStmt& s, const void* outer) {
    if (s.ctes.empty()) return selectChain(s, outer);
    std::deque<Table> store;
    const size_t base = cteStack_.size();
    struct Restore {
        std::vector<std::pair<std::string, Table*>>& st; size_t n;
        ~Restore() { st.resize(n); }
    } restore{ cteStack_, base };
    for (const CteDef& d : s.ctes) {
        std::vector<const SelectStmt*> chain;
        for (const SelectStmt* m = d.sel.get(); m; m = m->next.get()) chain.push_back(m);
        size_t firstRec = chain.size();
        if (s.recursive)
            for (size_t i = 0; i < chain.size(); ++i) if (refersTo(*chain[i], d.name)) { firstRec = i; break; }
        if (firstRec == chain.size()) {  // plain (non-recursive) CTE
            Result res = selectChain(*d.sel, nullptr);
            store.push_back(cteTable(d.name, d.cols, res));
            cteStack_.emplace_back(d.name, &store.back());
            continue;
        }
        if (firstRec == 0) throw SqlError(3575, "HY000", "Recursive Common Table Expression '" + d.name + "' should contain a UNION");
        // anchor = members before the first self-referencing one; run them as a UNION chain of their own
        bool distinct = !chain[firstRec - 1]->unionAll;
        Result acc;
        {
            Result first = selectOne(*chain[0], nullptr);
            acc = std::move(first);
            for (size_t i = 1; i < firstRec; ++i) {
                Result more = selectOne(*chain[i], nullptr);
                if (more.headers.size() != acc.headers.size())
                    throw SqlError(1222, "21000", "The used SELECT statements have a different number of columns");
                for (Row& row : more.rows) acc.rows.push_back(std::move(row));
            }
        }
        std::set<std::string> seen;
        auto keyRow = [&](const Row& row) { std::string k; for (const Value& v : row) { k += keyOf(v); k += '\x1f'; } return k; };
        if (distinct) {
            std::vector<Row> kept;
            for (Row& row : acc.rows) if (seen.insert(keyRow(row)).second) kept.push_back(std::move(row));
            acc.rows = std::move(kept);
        }
        std::vector<Row> total = acc.rows, working = acc.rows;
        store.push_back(cteTable(d.name, d.cols, acc));
        Table& ct = store.back();
        cteStack_.emplace_back(d.name, &ct);
        for (int iter = 0;; ++iter) {
            if (iter >= 1000)
                throw SqlError(3636, "HY000", "Recursive query aborted after 1001 iterations. Try increasing @@cte_max_recursion_depth to a larger value.");
            ct.rows = working;
            std::vector<Row> fresh;
            for (size_t i = firstRec; i < chain.size(); ++i) {
                Result more = selectOne(*chain[i], nullptr);
                if (more.headers.size() != ct.cols.size())
                    throw SqlError(1222, "21000", "The used SELECT statements have a different number of columns");
                for (Row& row : more.rows) {
                    if (distinct && !seen.insert(keyRow(row)).second) continue;
                    fresh.push_back(std::move(row));
                }
            }
            if (fresh.empty()) break;
            for (const Row& row : fresh) total.push_back(row);
            working = std::move(fresh);
        }
        ct.rows = std::move(total);
    }
    return selectChain(s, outer);
}

Result Engine::selectChain(const SelectStmt& s, const void* outer) {
    std::vector<const SelectStmt*> ch;
    for (const SelectStmt* cur = &s; cur; cur = cur->next.get()) ch.push_back(cur);
    auto rowKey = [](const Row& row) { std::string k; for (const Value& v : row) { k += keyOf(v); k += '\x1f'; } return k; };
    // l <op> r with MySQL semantics; op: 0 UNION, 1 INTERSECT, 2 EXCEPT; `all` keeps duplicates
    auto apply = [&](Result l, Result rr, int op, bool all) {
        if (rr.headers.size() != l.headers.size())
            throw SqlError(1222, "21000", "The used SELECT statements have a different number of columns");
        std::vector<Row> out;
        if (op == 0) {
            out = std::move(l.rows);
            for (Row& row : rr.rows) out.push_back(std::move(row));
            if (!all) {
                std::set<std::string> seen;
                std::vector<Row> kept;
                for (Row& row : out) if (seen.insert(rowKey(row)).second) kept.push_back(std::move(row));
                out = std::move(kept);
            }
        }
        else {
            std::map<std::string, int64_t> cnt;
            for (const Row& row : rr.rows) ++cnt[rowKey(row)];
            std::set<std::string> seen;
            for (Row& row : l.rows) {
                std::string k = rowKey(row);
                auto it = cnt.find(k);
                bool inR = it != cnt.end() && it->second > 0;
                if (all) {
                    if (op == 1) { if (inR) { --it->second; out.push_back(std::move(row)); } }
                    else if (inR) --it->second;
                    else out.push_back(std::move(row));
                }
                else if ((op == 1) == inR && seen.insert(k).second) out.push_back(std::move(row));
            }
        }
        l.rows = std::move(out);
        return l;
        };
    size_t i = 0;
    // INTERSECT binds tighter than UNION / EXCEPT (MySQL 8.0.31+)
    auto term = [&]() {
        Result t = selectOne(*ch[i], outer);
        while (ch[i]->next && ch[i]->setOp == 1) {
            bool all = ch[i]->unionAll;
            ++i;
            t = apply(std::move(t), selectOne(*ch[i], outer), 1, all);
        }
        return t;
        };
    Result r = term();
    while (ch[i]->next) {
        int op = ch[i]->setOp;
        bool all = ch[i]->unionAll;
        ++i;
        r = apply(std::move(r), term(), op, all);
    }
    if (s.next && (!s.uOrder.empty() || s.uLimit >= 0 || s.uOffset > 0)) {  // ORDER BY / LIMIT over the whole UNION
        Scope us;
        for (const std::string& h : r.headers) us.cols.push_back({ "", h, CType::Text });
        if (!s.uOrder.empty()) {
            std::vector<std::pair<std::vector<Value>, size_t>> ks;
            for (size_t i = 0; i < r.rows.size(); ++i) {
                std::vector<Value> k;
                for (const OrderItem& o : s.uOrder) {
                    if (o.e->k == EK::Lit && o.e->v.t == Value::T::Int) {
                        if (o.e->v.i < 1 || o.e->v.i > static_cast<int64_t>(r.headers.size()))
                            throw SqlError(1054, "42S22", "Unknown column '" + std::to_string(o.e->v.i) + "' in 'order clause'");
                        k.push_back(r.rows[i][static_cast<size_t>(o.e->v.i - 1)]);
                    }
                    else k.push_back(eval(*o.e, Ctx{ &us, &r.rows[i], nullptr, &env_, nullptr }));
                }
                ks.emplace_back(std::move(k), i);
            }
            std::stable_sort(ks.begin(), ks.end(), [&](const auto& x, const auto& y) { return keysLess(x.first, y.first, s.uOrder); });
            std::vector<Row> sorted;
            for (auto& kv : ks) sorted.push_back(std::move(r.rows[kv.second]));
            r.rows = std::move(sorted);
        }
        size_t off = static_cast<size_t>(std::max<int64_t>(0, s.uOffset));
        if (off >= r.rows.size()) r.rows.clear();
        else r.rows.erase(r.rows.begin(), r.rows.begin() + static_cast<long>(off));
        if (s.uLimit >= 0 && r.rows.size() > static_cast<size_t>(s.uLimit)) r.rows.resize(static_cast<size_t>(s.uLimit));
    }
    return r;
}

Result Engine::alter(Stmt& s) {
    const std::string& d = s.db.empty() ? env_.db : s.db;
    Table& t = table(s.db, s.name);
    Result r;
    if (!s.after.empty() && t.find(s.after) < 0) throw SqlError(1054, "42S22", "Unknown column '" + s.after + "' in '" + t.name + "'");
    if (s.op == "ADD") {
        const ColDef& cd = s.col;
        if (t.find(cd.name) >= 0) throw SqlError(1060, "42S21", "Duplicate column name '" + cd.name + "'");
        Column c;
        c.name = cd.name; c.typeName = cd.typeName; c.type = cd.type; c.len = cd.len; c.notNull = cd.notNull || cd.primary;
        c.autoInc = cd.autoInc; c.hasDef = cd.hasDef; c.defNow = cd.defNow; c.def = cd.def; c.gen = cd.gen; c.genStored = cd.genStored;
        Value fill = c.hasDef ? defaultFor(c, env_.now) : Value();
        if (fill.isNull() && c.notNull && !c.autoInc) fill = c.type == CType::Text ? Value::text("") : c.type == CType::Date ? Value::text("0000-00-00") : c.type == CType::DateTime ? Value::text("0000-00-00 00:00:00") : c.type == CType::Real ? Value::real(0) : Value::integer(0);
        std::vector<Row> backupRows = t.rows;
        size_t idx = t.cols.size();
        t.cols.push_back(c);
        t.invalidate();
        int64_t next = t.autoNext;
        for (Row& row : t.rows) row.push_back(c.autoInc ? Value::integer(next++) : fill);
        try {
            if (!c.gen.empty()) {
                try { Row probe(t.cols.size()); computeGenerated(t, probe, 0); }
                catch (const SqlError& e) { if (e.code == 1054) throw; }
                size_t rn = 0;
                for (Row& row : t.rows) computeGenerated(t, row, ++rn);
            }
            if (cd.primary) {
                if (t.hasPk()) throw SqlError(1068, "42000", "Multiple primary key defined");
                addUnique(t, "PRIMARY", { idx });
                std::rotate(t.uniques.begin(), t.uniques.end() - 1, t.uniques.end());
                t.invalidate();
            }
            if (cd.unique) addUnique(t, c.name, { idx });
        }
        catch (...) {
            t.cols.pop_back();
            t.rows = std::move(backupRows);
            t.invalidate();
            t.uniques.erase(std::remove_if(t.uniques.begin(), t.uniques.end(), [&](const UniqueKey& u) { return std::find(u.cols.begin(), u.cols.end(), idx) != u.cols.end(); }), t.uniques.end());
            throw;
        }
        if (c.autoInc) t.autoNext = next;
    }
    else if (s.op == "DROP") {
        int ci = t.find(s.newName);
        if (ci < 0) throw SqlError(1091, "42000", "Can't DROP '" + s.newName + "'; check that column/key exists");
        if (t.cols.size() == 1) throw SqlError(1090, "42000", "You can't delete all columns with ALTER TABLE; use DROP TABLE instead");
        size_t idx = static_cast<size_t>(ci);
        for (const ForeignKey& fk : t.fks)
            if (std::find(fk.cols.begin(), fk.cols.end(), idx) != fk.cols.end())
                throw SqlError(1828, "HY000", "Cannot drop column '" + s.newName + "': needed in a foreign key constraint '" + fk.name + "'");
        for (const FkChild& ch : fkChildren(d, t.name))
            for (const std::string& rc : ch.t->fks[ch.fk].refCols)
                if (ciCompare(rc, s.newName) == 0)
                    throw SqlError(1829, "HY000", "Cannot drop column '" + s.newName + "': needed in a foreign key constraint '" + ch.t->fks[ch.fk].name + "' of table '" + ch.db + "." + ch.t->name + "'");
        for (ForeignKey& fk : t.fks) for (size_t& x : fk.cols) if (x > idx) --x;
        t.cols.erase(t.cols.begin() + ci);
        for (Row& row : t.rows) row.erase(row.begin() + ci);
        std::vector<UniqueKey> keep;
        for (UniqueKey& u : t.uniques) {
            if (std::find(u.cols.begin(), u.cols.end(), idx) != u.cols.end()) continue;
            for (size_t& x : u.cols) if (x > idx) --x;
            keep.push_back(u);
        }
        t.uniques = std::move(keep);
        std::vector<UniqueKey> keepKeys;
        for (UniqueKey& u : t.keys) {
            if (std::find(u.cols.begin(), u.cols.end(), idx) != u.cols.end()) continue;
            for (size_t& x : u.cols) if (x > idx) --x;
            keepKeys.push_back(u);
        }
        t.keys = std::move(keepKeys);
        t.invalidate();
    }
    else if (s.op == "RENAME") {
        Database& db = database(d);
        if (db.tables.count(s.newName)) throw SqlError(1050, "42S01", "Table '" + s.newName + "' already exists");
        Table nt = std::move(t);
        db.tables.erase(s.name);
        nt.name = s.newName;
        db.tables[s.newName] = std::move(nt);
        for (auto& dp : cat_.dbs)  // keep foreign keys that reference this table pointing at it
            for (auto& tp : dp.second.tables)
                for (ForeignKey& fk : tp.second.fks)
                    if (ciCompare(fk.refDb, d) == 0 && ciCompare(fk.refTable, s.name) == 0) fk.refTable = s.newName;
    }
    else if (s.op == "UNIQUE") {
        std::vector<size_t> cols;
        for (const auto& n : s.idxCols) {
            int i = t.find(n);
            if (i < 0) throw SqlError(1072, "42000", "Key column '" + n + "' doesn't exist in table");
            cols.push_back(static_cast<size_t>(i));
        }
        addUnique(t, s.newName, cols);
    }
    else if (s.op == "MODIFY" || s.op == "CHANGE") {
        const ColDef& cd = s.col;
        const std::string oldName = s.op == "CHANGE" ? s.newName : cd.name;
        if (int fci = t.find(oldName); fci >= 0) {
            for (const ForeignKey& fk : t.fks)
                if (std::find(fk.cols.begin(), fk.cols.end(), static_cast<size_t>(fci)) != fk.cols.end())
                    throw SqlError(1832, "HY000", "Cannot change column '" + oldName + "': used in a foreign key constraint '" + fk.name + "'");
            for (const FkChild& ch : fkChildren(d, t.name))
                for (const std::string& rc : ch.t->fks[ch.fk].refCols)
                    if (ciCompare(rc, oldName) == 0)
                        throw SqlError(1833, "HY000", "Cannot change column '" + oldName + "': used in a foreign key constraint '" + ch.t->fks[ch.fk].name + "' of table '" + ch.db + "." + ch.t->name + "'");
        }
        int ci = t.find(oldName);
        if (ci < 0) throw SqlError(1054, "42S22", "Unknown column '" + oldName + "' in '" + t.name + "'");
        size_t idx = static_cast<size_t>(ci);
        if (s.op == "CHANGE" && ciCompare(cd.name, oldName) != 0 && t.find(cd.name) >= 0)
            throw SqlError(1060, "42S21", "Duplicate column name '" + cd.name + "'");
        Column nc;
        nc.name = cd.name; nc.typeName = cd.typeName; nc.type = cd.type; nc.len = cd.len;
        nc.autoInc = cd.autoInc; nc.hasDef = cd.hasDef; nc.defNow = cd.defNow; nc.def = cd.def; nc.gen = cd.gen; nc.genStored = cd.genStored;
        nc.notNull = cd.notNull || (t.hasPk() && std::find(t.uniques[0].cols.begin(), t.uniques[0].cols.end(), idx) != t.uniques[0].cols.end());
        std::vector<Row> newRows = t.rows;
        int64_t maxId = 0;
        for (size_t ri = 0; ri < newRows.size(); ++ri) {
            Value& v = newRows[ri][idx];
            if (v.isNull()) {
                if (nc.notNull) throw SqlError(1138, "22004", "Invalid use of NULL value");
                continue;
            }
            v = coerce(nc, v, ri + 1);
            if (v.t == Value::T::Int) maxId = std::max(maxId, v.i);
        }
        Column oldCol = t.cols[idx];
        std::vector<Row> oldRows = std::move(t.rows);
        t.cols[idx] = nc;
        t.rows = std::move(newRows);
        try {
            for (const UniqueKey& u : t.uniques) {
                if (std::find(u.cols.begin(), u.cols.end(), idx) == u.cols.end()) continue;
                IndexMap seen;
                for (const Row& row : t.rows) {
                    std::vector<Value> k;
                    if (t.makeKey(u, row, k) && !seen.emplace(k, std::vector<size_t>{0}).second) throw dupError(u, row);
                }
            }
        }
        catch (...) {
            t.cols[idx] = oldCol;
            t.rows = std::move(oldRows);
            t.invalidate();
            throw;
        }
        if (nc.autoInc && !oldCol.autoInc) t.autoNext = std::max(t.autoNext, maxId + 1);
        t.invalidate();
    }
    else if (s.op == "RENAMECOL") {
        int ci = t.find(s.newName);
        if (ci < 0) throw SqlError(1054, "42S22", "Unknown column '" + s.newName + "' in '" + t.name + "'");
        if (ciCompare(s.col.name, s.newName) != 0 && t.find(s.col.name) >= 0)
            throw SqlError(1060, "42S21", "Duplicate column name '" + s.col.name + "'");
        for (const FkChild& ch : fkChildren(d, t.name))  // referencing keys follow the rename
            for (std::string& rc : ch.t->fks[ch.fk].refCols) if (ciCompare(rc, s.newName) == 0) rc = s.col.name;
        t.cols[static_cast<size_t>(ci)].name = s.col.name;
    }
    else if (s.op == "ADDFK") {
        t.fks.push_back(makeFk(s.fks[0], t, d, t.fks.size() + 1));
        try { for (const Row& row : t.rows) fkCheckChild(t, d, row); }
        catch (...) { t.fks.pop_back(); throw; }
    }
    else if (s.op == "ADDCHECK") {
        std::string nm = s.checks[0].first.empty() ? t.name + "_chk_" + std::to_string(t.checks.size() + 1) : s.checks[0].first;
        for (const CheckDef& o : t.checks) if (ciCompare(o.name, nm) == 0) throw SqlError(3822, "HY000", "Duplicate check constraint name '" + nm + "'.");
        t.checks.push_back({ nm, s.checks[0].second });
        try {
            try { checkRow(t, Row(t.cols.size())); }
            catch (const SqlError& e) { if (e.code != 3819) throw; }
            for (const Row& row : t.rows) checkRow(t, row);
        }
        catch (...) { t.checks.pop_back(); throw; }
    }
    else if (s.op == "DROPFK") {
        auto it = std::find_if(t.fks.begin(), t.fks.end(), [&](const ForeignKey& f) { return ciCompare(f.name, s.newName) == 0; });
        auto ck = std::find_if(t.checks.begin(), t.checks.end(), [&](const CheckDef& c) { return ciCompare(c.name, s.newName) == 0; });
        if (it != t.fks.end()) t.fks.erase(it);
        else if (ck != t.checks.end()) t.checks.erase(ck);
        else throw SqlError(1091, "42000", "Can't DROP '" + s.newName + "'; check that column/key exists");
    }
    else if (s.op == "PK") {
        if (t.hasPk()) throw SqlError(1068, "42000", "Multiple primary key defined");
        std::vector<size_t> cols;
        for (const auto& n : s.idxCols) {
            int i = t.find(n);
            if (i < 0) throw SqlError(1072, "42000", "Key column '" + n + "' doesn't exist in table");
            for (const Row& row : t.rows) if (row[static_cast<size_t>(i)].isNull()) throw SqlError(1138, "22004", "Invalid use of NULL value");
            cols.push_back(static_cast<size_t>(i));
        }
        addUnique(t, "PRIMARY", cols);
        std::rotate(t.uniques.begin(), t.uniques.end() - 1, t.uniques.end());
        for (size_t c : cols) t.cols[c].notNull = true;
        t.invalidate();
    }
    else if (s.op == "DROPPK") {
        if (!t.hasPk()) throw SqlError(1091, "42000", "Can't DROP 'PRIMARY'; check that column/key exists");
        t.uniques.erase(t.uniques.begin());
        t.invalidate();
    }
    else if (s.op == "DROPINDEX") {
        bool found = false;
        for (size_t i = 0; i < t.uniques.size() && !found; ++i)
            if (t.uniques[i].name != "PRIMARY" && ciCompare(t.uniques[i].name, s.newName) == 0) { t.uniques.erase(t.uniques.begin() + static_cast<long>(i)); found = true; }
        for (size_t i = 0; i < t.keys.size() && !found; ++i)
            if (ciCompare(t.keys[i].name, s.newName) == 0) { t.keys.erase(t.keys.begin() + static_cast<long>(i)); found = true; }
        if (!found) throw SqlError(1091, "42000", "Can't DROP '" + s.newName + "'; check that column/key exists");
        t.invalidate();
    }
    else if (s.op == "INDEX") {
        std::vector<size_t> cols;
        for (const auto& n : s.idxCols) {
            int i = t.find(n);
            if (i < 0) throw SqlError(1072, "42000", "Key column '" + n + "' doesn't exist in table");
            cols.push_back(static_cast<size_t>(i));
        }
        t.keys.push_back({ s.newName, cols });
        t.invalidate();
    }
    if ((s.first || !s.after.empty()) && (s.op == "ADD" || s.op == "MODIFY" || s.op == "CHANGE")) {
        const int from = t.find(s.col.name);
        std::vector<size_t> order;  // order[newPos] = old index
        for (size_t i = 0; i < t.cols.size(); ++i) if (static_cast<int>(i) != from) order.push_back(i);
        size_t pos = 0;
        if (!s.first) {
            const int a = t.find(s.after);
            pos = static_cast<size_t>(std::find(order.begin(), order.end(), static_cast<size_t>(a)) - order.begin()) + 1;
        }
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<size_t>(from));
        std::vector<size_t> newIdx(order.size());
        for (size_t i = 0; i < order.size(); ++i) newIdx[order[i]] = i;
        std::vector<Column> nc;
        for (size_t i : order) nc.push_back(t.cols[i]);
        t.cols = std::move(nc);
        for (Row& row : t.rows) { Row nr; for (size_t i : order) nr.push_back(std::move(row[i])); row = std::move(nr); }
        for (UniqueKey& u : t.uniques) for (size_t& x : u.cols) x = newIdx[x];
        for (UniqueKey& u : t.keys) for (size_t& x : u.cols) x = newIdx[x];
        for (ForeignKey& f : t.fks) for (size_t& x : f.cols) x = newIdx[x];
        t.invalidate();
    }
    r.msg = "Query OK, 0 rows affected";
    r.info = "Records: 0  Duplicates: 0  Warnings: 0";
    return r;
}

Result Engine::explain(const SelectStmt& s) {
    Result r;
    r.isSet = true;
    r.headers = { "id", "select_type", "table", "type", "possible_keys", "key", "rows", "Extra" };
    Value one = Value::integer(1), simple = Value::text("SIMPLE");
    if (!s.hasFrom) {
        r.rows.push_back({ one, simple, Value(), Value(), Value(), Value(), Value(), Value::text("No tables used") });
        return r;
    }
    auto joinNames = [](const std::vector<std::string>& v) {
        std::string o;
        for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + v[i];
        return o.empty() ? Value() : Value::text(o);
        };
    std::deque<Table> temps;
    const Table& t0 = *tableFor(s.from, temps);
    Scope sc = scopeFor(t0, s.from.alias.empty() ? t0.name : s.from.alias);
    IndexPlan ip = planIndex(t0, sc, s.where.get(), !s.joins.empty(), &env_, nullptr, false);
    r.rows.push_back({ one, simple, Value::text(t0.name), Value::text(ip.used ? ip.type : "ALL"), joinNames(ip.possible),
                      ip.used ? Value::text(t0.indexDef(ip.index).name) : Value(),
                      Value::integer(static_cast<int64_t>(ip.used ? ip.rows.size() : t0.rows.size())),
                      (!ip.used && s.where) ? Value::text("Using where") : Value() });
    Scope cur = sc;
    for (const JoinClause& j : s.joins) {
        const Table& tr = *relation(j.ref.db, j.ref.name, temps);
        Scope rs = scopeFor(tr, j.ref.alias.empty() ? tr.name : j.ref.alias);
        int lk = -1, rk = -1;
        if (j.on) hashKeys(*j.on, cur, rs, lk, rk);
        std::string type = "ALL", key, extra = "Using join buffer (nested loop)";
        if (lk >= 0) {
            extra = "Using hash join";
            for (size_t i = 0; i < tr.nIndexes(); ++i)
                if (tr.indexDef(i).cols[0] == static_cast<size_t>(rk)) { type = "ref"; key = tr.indexDef(i).name; extra.clear(); break; }
        }
        r.rows.push_back({ one, simple, Value::text(tr.name), Value::text(type), Value(), key.empty() ? Value() : Value::text(key),
                          Value::integer(static_cast<int64_t>(tr.rows.size())), extra.empty() ? Value() : Value::text(extra) });
        cur.cols.insert(cur.cols.end(), rs.cols.begin(), rs.cols.end());
    }
    return r;
}