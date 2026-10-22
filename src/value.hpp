#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

// MySQL-style error: numeric code, SQLSTATE, message.
struct SqlError : std::runtime_error {
    int code;
    std::string state;
    SqlError(int c, std::string st, const std::string& m)
        : std::runtime_error(m), code(c), state(std::move(st)) {}
};

// New enumerators go at the END: the snapshot file stores this as an int.
enum class CType { Int, Real, Text, Date, DateTime };

struct Value {
    enum class T { Null, Int, Real, Text };
    T t = T::Null;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    int scale = -1;  // Real only: >= 0 means exact DECIMAL with this many fractional digits

    static Value integer(int64_t v) { Value x; x.t = T::Int; x.i = v; return x; }
    static Value real(double v) { Value x; x.t = T::Real; x.d = v; return x; }
    // DECIMAL: rounded to `sc` places, printed with exactly `sc` places, arithmetic keeps exactness rules.
    static Value dec(double v, int sc) {
        double m = std::pow(10.0, sc), r = std::round(v * m) / m;
        if (r == 0) r = 0.0;  // no negative zero
        Value x;
        x.t = T::Real; x.d = r; x.scale = sc;
        return x;
    }
    static Value text(std::string v) { Value x; x.t = T::Text; x.s = std::move(v); return x; }

    bool isNull() const { return t == T::Null; }
    bool isNum() const { return t == T::Int || t == T::Real; }

    double asReal() const {
        switch (t) {
        case T::Int: return static_cast<double>(i);
        case T::Real: return d;
        case T::Text: return std::strtod(s.c_str(), nullptr);
        case T::Null: break;
        }
        return 0.0;
    }
    int64_t asInt() const {
        switch (t) {
        case T::Int: return i;
        case T::Real: return static_cast<int64_t>(std::llround(d));
        case T::Text: return static_cast<int64_t>(std::llround(std::strtod(s.c_str(), nullptr)));
        case T::Null: break;
        }
        return 0;
    }
    bool truthy() const { return !isNull() && asReal() != 0.0; }

    std::string str() const {
        switch (t) {
        case T::Null: return "NULL";
        case T::Int: return std::to_string(i);
        case T::Real: {
            if (std::isnan(d)) return "NaN";
            char b[128];
            if (scale >= 0) std::snprintf(b, sizeof b, "%.*f", scale, d);
            else std::snprintf(b, sizeof b, "%.15g", d);
            return b;
        }
        case T::Text: return s;
        }
        return {};
    }
};

inline int ciCompare(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t k = 0; k < n; ++k) {
        int x = std::tolower(static_cast<unsigned char>(a[k]));
        int y = std::tolower(static_cast<unsigned char>(b[k]));
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// Both operands must be non-NULL. Text vs text is case-insensitive (MySQL's default collation).
inline int compareValues(const Value& a, const Value& b) {
    if (a.t == Value::T::Text && b.t == Value::T::Text) return ciCompare(a.s, b.s);
    if (a.t == Value::T::Int && b.t == Value::T::Int) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    double x = a.asReal(), y = b.asReal();
    return x < y ? -1 : (x > y ? 1 : 0);
}

inline bool sameExact(const Value& a, const Value& b) {
    if (a.t != b.t) return false;
    switch (a.t) {
    case Value::T::Null: return true;
    case Value::T::Int: return a.i == b.i;
    case Value::T::Real: return a.d == b.d;
    case Value::T::Text: return a.s == b.s;
    }
    return false;
}