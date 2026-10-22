#include "executor.hpp"

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

    bool isAgg(const std::string& n) { return n == "COUNT" || n == "SUM" || n == "AVG" || n == "MIN" || n == "MAX" || n == "GROUP_CONCAT"; }

    bool hasAgg(const Expr& e) {
        if (e.k == EK::Func && isAgg(e.name)) return true;
        for (const auto& a : e.args) if (hasAgg(*a)) return true;
        return false;
    }

    bool likeMatch(const std::string& s, size_t si, const std::string& p, size_t pi) {
        while (pi < p.size()) {
            char c = p[pi];
            if (c == '%') {
                while (pi < p.size() && p[pi] == '%') ++pi;
                if (pi == p.size()) return true;
                for (size_t k = si; k <= s.size(); ++k) if (likeMatch(s, k, p, pi)) return true;
                return false;
            }
            if (si >= s.size()) return false;
            if (c == '\\' && pi + 1 < p.size()) { ++pi; c = p[pi]; if (std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(s[si]))) return false; }
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
        if (n == "@@VAR") return sysVar(e.table);
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
            bool m = likeMatch(v.str(), 0, p.str(), 0);
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
    if (file_.empty()) return;
    cat_.load(file_);
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

const Table* Engine::relation(const std::string& db, const std::string& name, std::deque<Table>& temps) {
    const std::string& d = db.empty() ? env_.db : db;
    if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
    Database& dbase = database(d);
    auto it = dbase.tables.find(name);
    if (it != dbase.tables.end()) return &it->second;
    auto vt = dbase.views.find(name);
    if (vt == dbase.views.end()) throw SqlError(1146, "42S02", "Table '" + d + "." + name + "' doesn't exist");
    if (viewDepth_ >= 16) throw SqlError(1462, "HY000", "View '" + d + "." + name + "' has a recursive definition");
    std::string savedDb = env_.db;
    env_.db = d;  // views resolve their tables in their own database
    ++viewDepth_;
    Result res;
    try {
        Stmt vs = parseStatement(vt->second.sql);
        res = select(*vs.sel);
    }
    catch (...) {
        --viewDepth_;
        env_.db = savedDb;
        throw;
    }
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
    case SK::Use: database(s.name); env_.db = s.name; r.msg = "Database changed"; r.timing = false; break;
    case SK::ShowDbs:
        r.isSet = true; r.headers = { "Database" };
        for (const auto& d : cat_.dbs) r.rows.push_back({ Value::text(d.first) });
        break;
    case SK::ShowTables: {
        const std::string& d = s.db.empty() ? env_.db : s.db;
        if (d.empty()) throw SqlError(1046, "3D000", "No database selected");
        r.isSet = true; r.headers = { "Tables_in_" + d };
        std::set<std::string> names;
        for (const auto& t : database(d).tables) names.insert(t.first);
        for (const auto& v : database(d).views) names.insert(v.first);
        for (const std::string& n : names) r.rows.push_back({ Value::text(n) });
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
        else { db.tables.erase(it); r.msg = affected(0); mutated = true; }
        break;
    }
    case SK::Truncate: {
        Table& t = table(s.db, s.name);
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
    if (mutated && !replaying_ && !file_.empty()) pending_.push_back({ static_cast<int64_t>(env_.now), dbAtStart, sql });
    if (!inTxn_) flushLog();
    return r;
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
        c.autoInc = cd.autoInc; c.hasDef = cd.hasDef; c.defNow = cd.defNow; c.def = cd.def;
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
    for (const auto& uk : s.uniqueKeys) {
        UniqueKey u;
        u.name = uk[0];
        for (const auto& n : uk) u.cols.push_back(idxOf(n));
        t.uniques.push_back(u);
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
    auto addRow = [&](const std::vector<Value>& vals, const std::vector<bool>& dflt, size_t rowNo) {
        if (vals.size() != colIdx.size())
            throw SqlError(1136, "21S01", "Column count doesn't match value count at row " + std::to_string(rowNo));
        Row r(t.cols.size());
        std::vector<bool> given(t.cols.size(), false);
        for (size_t k = 0; k < vals.size(); ++k) if (!dflt[k]) { r[colIdx[k]] = vals[k]; given[colIdx[k]] = true; }
        for (size_t ci = 0; ci < t.cols.size(); ++ci) {
            const Column& c = t.cols[ci];
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
        for (size_t u = 0; u < t.uniques.size(); ++u) {
            std::vector<Value> k;
            if (t.makeKey(t.uniques[u], r, k) && t.idx[u].count(k)) throw dupError(t.uniques[u], r);
        }
        t.rows.push_back(std::move(r));
        t.indexAdd(t.rows.back(), t.rows.size() - 1);
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
        t.rows.resize(orig);
        t.invalidate();
        throw;
    }
    if (idSet) env_.lastId = firstId;
    int64_t n = static_cast<int64_t>(t.rows.size() - orig);
    Result r;
    r.msg = "Query OK, " + std::to_string(n) + (n == 1 ? " row" : " rows") + " affected";
    if (n != 1) r.info = "Records: " + std::to_string(n) + "  Duplicates: 0  Warnings: 0";
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
    try {
        for (size_t ri : match) {
            Row old = t.rows[ri], nw = old;
            Ctx c{ &sc, &old, nullptr, &env_, nullptr };
            for (size_t k = 0; k < setIdx.size(); ++k) {
                const Column& col = t.cols[setIdx[k]];
                Value v = coerce(col, eval(*s.sets[k].second, c), ri + 1);
                if (v.isNull() && col.notNull) throw SqlError(1048, "23000", "Column '" + col.name + "' cannot be null");
                nw[setIdx[k]] = v;
            }
            bool same = true;
            for (size_t k = 0; k < nw.size(); ++k) if (!sameExact(nw[k], old[k])) same = false;
            if (!same) {
                for (size_t u = 0; u < t.uniques.size(); ++u) {
                    std::vector<Value> k;
                    if (!t.makeKey(t.uniques[u], nw, k)) continue;
                    auto it = t.idx[u].find(k);
                    if (it != t.idx[u].end()) for (size_t other : it->second) if (other != ri) throw dupError(t.uniques[u], nw);
                }
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
        throw;
    }
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
    std::vector<Row> keep;
    for (size_t i = 0; i < t.rows.size(); ++i) if (!kill[i]) keep.push_back(std::move(t.rows[i]));
    t.rows = std::move(keep);
    t.invalidate();
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
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column& c = t.cols[i];
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
        r.rows.push_back({ Value::text(c.name), Value::text(c.typeName), Value::text(c.notNull ? "NO" : "YES"),
                          Value::text(key), def, Value::text(c.autoInc ? "auto_increment" : "") });
    }
    return r;
}

Result Engine::showCreate(const Stmt& s) {
    Table& t = table(s.db, s.name);
    std::string o = "CREATE TABLE `" + t.name + "` (\n";
    for (size_t i = 0; i < t.cols.size(); ++i) {
        const Column& c = t.cols[i];
        o += "  `" + c.name + "` " + c.typeName;
        if (c.notNull) o += " NOT NULL";
        if (c.autoInc) o += " AUTO_INCREMENT";
        if (c.hasDef && c.defNow) o += " DEFAULT CURRENT_TIMESTAMP";
        else if (c.hasDef && !c.def.isNull()) o += " DEFAULT " + quoteLit(c.def.str());
        else if (!c.notNull) o += " DEFAULT NULL";
        o += (i + 1 < t.cols.size() || t.nIndexes() > 0) ? ",\n" : "\n";
    }
    for (size_t u = 0; u < t.nIndexes(); ++u) {
        const UniqueKey& k = t.indexDef(u);
        o += k.name == "PRIMARY" ? "  PRIMARY KEY (" : (u < t.uniques.size() ? "  UNIQUE KEY `" : "  KEY `") + k.name + "` (";
        for (size_t j = 0; j < k.cols.size(); ++j) o += (j ? ",`" : "`") + t.cols[k.cols[j]].name + "`";
        o += u + 1 < t.nIndexes() ? "),\n" : ")\n";
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
        const Table& t0 = *relation(s.from.db, s.from.name, temps);
        sc = scopeFor(t0, s.from.alias.empty() ? t0.name : s.from.alias);
        IndexPlan ip = planIndex(t0, sc, s.where.get(), !s.joins.empty(), &env_, outerCtx, s.joins.empty() && outerCtx != nullptr);
        if (ip.used) { rows.reserve(ip.rows.size()); for (size_t id : ip.rows) rows.push_back(t0.rows[id]); }
        else rows = t0.rows;
        for (const JoinClause& j : s.joins) {
            const Table& tr = *relation(j.ref.db, j.ref.name, temps);
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
    auto emit = [&](const Row* row, const std::vector<const Row*>* group) {
        Ent en;
        Ctx c{ &sc, row, group, &env_, nullptr, outerCtx };
        for (const Out& o : outs) en.out.push_back(o.e ? eval(*o.e, c) : (row ? (*row)[o.idx] : Value()));
        Ctx oc{ &sc, row, group, &env_, &s.items, outerCtx };
        for (size_t i = 0; i < s.order.size(); ++i)
            en.keys.push_back(okIdx[i] >= 0 ? en.out[static_cast<size_t>(okIdx[i])] : eval(*s.order[i].e, oc));
        ents.push_back(std::move(en));
        };
    if (!agg) {
        for (const Row* r : src) {
            if (s.having && !eval(*s.having, Ctx{ &sc, r, nullptr, &env_, &s.items, outerCtx }).truthy()) continue;
            emit(r, nullptr);
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
        std::vector<std::vector<const Row*>> groups;
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
            emit(first, &g);
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

Result Engine::select(const SelectStmt& s, const void* outer) {
    Result r = selectOne(s, outer);
    for (const SelectStmt* cur = &s; cur->next; cur = cur->next.get()) {
        Result n = selectOne(*cur->next, outer);
        if (n.headers.size() != r.headers.size())
            throw SqlError(1222, "21000", "The used SELECT statements have a different number of columns");
        for (Row& row : n.rows) r.rows.push_back(std::move(row));
        if (!cur->unionAll) {
            std::set<std::string> seen;
            std::vector<Row> kept;
            for (Row& row : r.rows) {
                std::string k;
                for (const Value& v : row) { k += keyOf(v); k += '\x1f'; }
                if (seen.insert(k).second) kept.push_back(std::move(row));
            }
            r.rows = std::move(kept);
        }
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
    if (s.op == "ADD") {
        const ColDef& cd = s.col;
        if (t.find(cd.name) >= 0) throw SqlError(1060, "42S21", "Duplicate column name '" + cd.name + "'");
        Column c;
        c.name = cd.name; c.typeName = cd.typeName; c.type = cd.type; c.len = cd.len; c.notNull = cd.notNull || cd.primary;
        c.autoInc = cd.autoInc; c.hasDef = cd.hasDef; c.defNow = cd.defNow; c.def = cd.def;
        Value fill = c.hasDef ? defaultFor(c, env_.now) : Value();
        if (fill.isNull() && c.notNull && !c.autoInc) fill = c.type == CType::Text ? Value::text("") : c.type == CType::Date ? Value::text("0000-00-00") : c.type == CType::DateTime ? Value::text("0000-00-00 00:00:00") : c.type == CType::Real ? Value::real(0) : Value::integer(0);
        std::vector<Row> backupRows = t.rows;
        size_t idx = t.cols.size();
        t.cols.push_back(c);
        t.invalidate();
        int64_t next = t.autoNext;
        for (Row& row : t.rows) row.push_back(c.autoInc ? Value::integer(next++) : fill);
        try {
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
        int ci = t.find(oldName);
        if (ci < 0) throw SqlError(1054, "42S22", "Unknown column '" + oldName + "' in '" + t.name + "'");
        size_t idx = static_cast<size_t>(ci);
        if (s.op == "CHANGE" && ciCompare(cd.name, oldName) != 0 && t.find(cd.name) >= 0)
            throw SqlError(1060, "42S21", "Duplicate column name '" + cd.name + "'");
        Column nc;
        nc.name = cd.name; nc.typeName = cd.typeName; nc.type = cd.type; nc.len = cd.len;
        nc.autoInc = cd.autoInc; nc.hasDef = cd.hasDef; nc.defNow = cd.defNow; nc.def = cd.def;
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
        t.cols[static_cast<size_t>(ci)].name = s.col.name;
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
    const Table& t0 = *relation(s.from.db, s.from.name, temps);
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