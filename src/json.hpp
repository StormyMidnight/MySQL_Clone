// Minimal JSON documentmodel for the JSON column type and JSON_* functions.
// Output format follows MySQL: {"a": 1, "b": [1, 2]}; object keys sorted by (length, bytes).
#pragma once
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace jsn {

    struct J {
        enum class T { Null, Bool, Int, Real, Str, Arr, Obj } t = T::Null;
        bool b = false;
        int64_t i = 0;
        double d = 0;
        std::string s;
        std::vector<J> a;
        std::vector<std::pair<std::string, J>> o;
        static J str(std::string v) { J j; j.t = T::Str; j.s = std::move(v); return j; }
        static J integer(int64_t v) { J j; j.t = T::Int; j.i = v; return j; }
        static J real(double v) { J j; j.t = T::Real; j.d = v; return j; }
        static J arr() { J j; j.t = T::Arr; return j; }
        static J obj() { J j; j.t = T::Obj; return j; }
    };

    inline bool keyLess(const std::string& x, const std::string& y) { return x.size() != y.size() ? x.size() < y.size() : x < y; }

    inline void sortKeys(J& j) {
        std::stable_sort(j.o.begin(), j.o.end(), [](const auto& x, const auto& y) { return keyLess(x.first, y.first); });
    }

    // Sets member k (replacing an existing one) keeping the key order.
    inline void objSet(J& obj, const std::string& k, J v) {
        for (auto& m : obj.o) if (m.first == k) { m.second = std::move(v); return; }
        obj.o.emplace_back(k, std::move(v));
        sortKeys(obj);
    }

    struct Parser {
        const std::string& s;
        size_t p = 0;
        explicit Parser(const std::string& str) : s(str) {}
        void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p; }
        static void utf8(std::string& o, unsigned cp) {
            if (cp < 0x80) o += static_cast<char>(cp);
            else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        }
        bool hex4(unsigned& v) {
            if (p + 4 > s.size()) return false;
            v = 0;
            for (int k = 0; k < 4; ++k) {
                char c = s[p++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
                else return false;
            }
            return true;
        }
        bool string(std::string& out) {
            if (p >= s.size() || s[p] != '"') return false;
            ++p;
            while (p < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[p++]);
                if (c == '"') return true;
                if (c < 0x20) return false;
                if (c != '\\') { out += static_cast<char>(c); continue; }
                if (p >= s.size()) return false;
                char e = s[p++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        unsigned lo = 0;
                        if (p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') { p += 2; if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
                        else return false;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false;
        }
        bool value(J& j, int depth) {
            if (depth > 100) return false;
            ws();
            if (p >= s.size()) return false;
            char c = s[p];
            if (c == '{') {
                ++p; j = J::obj(); ws();
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                for (;;) {
                    ws();
                    std::string k;
                    if (!string(k)) return false;
                    ws();
                    if (p >= s.size() || s[p] != ':') return false;
                    ++p;
                    J v;
                    if (!value(v, depth + 1)) return false;
                    objSet(j, k, std::move(v));
                    ws();
                    if (p < s.size() && s[p] == ',') { ++p; continue; }
                    if (p < s.size() && s[p] == '}') { ++p; return true; }
                    return false;
                }
            }
            if (c == '[') {
                ++p; j = J::arr(); ws();
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                for (;;) {
                    J v;
                    if (!value(v, depth + 1)) return false;
                    j.a.push_back(std::move(v));
                    ws();
                    if (p < s.size() && s[p] == ',') { ++p; continue; }
                    if (p < s.size() && s[p] == ']') { ++p; return true; }
                    return false;
                }
            }
            if (c == '"') { j = J::str(""); return string(j.s); }
            if (s.compare(p, 4, "true") == 0) { p += 4; j = J(); j.t = J::T::Bool; j.b = true; return true; }
            if (s.compare(p, 5, "false") == 0) { p += 5; j = J(); j.t = J::T::Bool; j.b = false; return true; }
            if (s.compare(p, 4, "null") == 0) { p += 4; j = J(); return true; }
            size_t st = p;
            bool real = false;
            if (p < s.size() && s[p] == '-') ++p;
            if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return false;
            if (s[p] == '0' && p + 1 < s.size() && s[p + 1] >= '0' && s[p + 1] <= '9') return false;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
            if (p < s.size() && s[p] == '.') { real = true; ++p; size_t f = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (p == f) return false; }
            if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
                real = true; ++p;
                if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
                size_t f = p;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
                if (p == f) return false;
            }
            std::string num = s.substr(st, p - st);
            if (!real) {
                errno = 0;
                long long v = std::strtoll(num.c_str(), nullptr, 10);
                if (errno != ERANGE) { j = J::integer(v); return true; }
            }
            j = J::real(std::strtod(num.c_str(), nullptr));
            return true;
        }
    };

    inline bool parse(const std::string& text, J& out) {
        Parser ps(text);
        if (!ps.value(out, 0)) return false;
        ps.ws();
        return ps.p == text.size();
    }

    inline void quote(const std::string& s, std::string& o) {
        o += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
            }
        }
        o += '"';
    }

    inline std::string realStr(double d) {
        char b[64];
        for (int prec = 15; prec <= 17; ++prec) {
            std::snprintf(b, sizeof b, "%.*g", prec, d);
            if (std::strtod(b, nullptr) == d) break;
        }
        std::string r = b;
        if (r.find_first_of(".eEn") == std::string::npos) r += ".0";
        return r;
    }

    inline void dump(const J& j, std::string& o) {
        switch (j.t) {
        case J::T::Null: o += "null"; break;
        case J::T::Bool: o += j.b ? "true" : "false"; break;
        case J::T::Int: o += std::to_string(j.i); break;
        case J::T::Real: o += realStr(j.d); break;
        case J::T::Str: quote(j.s, o); break;
        case J::T::Arr:
            o += '[';
            for (size_t k = 0; k < j.a.size(); ++k) { if (k) o += ", "; dump(j.a[k], o); }
            o += ']';
            break;
        case J::T::Obj:
            o += '{';
            for (size_t k = 0; k < j.o.size(); ++k) { if (k) o += ", "; quote(j.o[k].first, o); o += ": "; dump(j.o[k].second, o); }
            o += '}';
            break;
        }
    }
    inline std::string dump(const J& j) { std::string o; dump(j, o); return o; }

    // ---- paths: $ .key ."key" [n] [*] .* **
    struct Step {
        enum class K { Key, Idx, AnyKey, AnyIdx, Rec } k = K::Key;
        std::string key;
        int64_t idx = 0;
    };

    inline bool parsePath(const std::string& p, std::vector<Step>& out) {
        size_t i = 0;
        while (i < p.size() && p[i] == ' ') ++i;
        if (i >= p.size() || p[i] != '$') return false;
        ++i;
        while (i < p.size()) {
            if (p[i] == ' ') { ++i; continue; }
            Step st;
            if (p[i] == '.') {
                ++i;
                if (i < p.size() && p[i] == '*') { st.k = Step::K::AnyKey; ++i; }
                else if (i < p.size() && p[i] == '"') {
                    Parser ps(p);
                    ps.p = i;
                    if (!ps.string(st.key)) return false;
                    i = ps.p;
                }
                else {
                    size_t b = i;
                    while (i < p.size() && (std::isalnum(static_cast<unsigned char>(p[i])) || p[i] == '_' || p[i] == '$' || static_cast<unsigned char>(p[i]) >= 0x80)) ++i;
                    if (i == b) return false;
                    st.key = p.substr(b, i - b);
                }
            }
            else if (p[i] == '[') {
                ++i;
                if (i < p.size() && p[i] == '*') { st.k = Step::K::AnyIdx; ++i; }
                else {
                    size_t b = i;
                    while (i < p.size() && std::isdigit(static_cast<unsigned char>(p[i]))) ++i;
                    if (i == b) return false;
                    st.k = Step::K::Idx;
                    st.idx = std::atoll(p.substr(b, i - b).c_str());
                }
                if (i >= p.size() || p[i] != ']') return false;
                ++i;
            }
            else if (p.compare(i, 2, "**") == 0) { st.k = Step::K::Rec; i += 2; }
            else return false;
            out.push_back(std::move(st));
        }
        return true;
    }

    inline bool wildcard(const std::vector<Step>& st) {
        for (const Step& s : st) if (s.k == Step::K::AnyKey || s.k == Step::K::AnyIdx || s.k == Step::K::Rec) return true;
        return false;
    }

    inline void select(const J& j, const std::vector<Step>& st, size_t i, std::vector<const J*>& out) {
        if (i == st.size()) { out.push_back(&j); return; }
        const Step& s = st[i];
        switch (s.k) {
        case Step::K::Key:
            if (j.t == J::T::Obj) for (const auto& m : j.o) if (m.first == s.key) { select(m.second, st, i + 1, out); break; }
            break;
        case Step::K::Idx:
            if (j.t == J::T::Arr) { if (s.idx >= 0 && static_cast<size_t>(s.idx) < j.a.size()) select(j.a[static_cast<size_t>(s.idx)], st, i + 1, out); }
            else if (s.idx == 0) select(j, st, i + 1, out);  // scalars autowrap as one-element arrays
            break;
        case Step::K::AnyKey:
            if (j.t == J::T::Obj) for (const auto& m : j.o) select(m.second, st, i + 1, out);
            break;
        case Step::K::AnyIdx:
            if (j.t == J::T::Arr) for (const J& e : j.a) select(e, st, i + 1, out);
            break;
        case Step::K::Rec:
            select(j, st, i + 1, out);
            if (j.t == J::T::Obj) for (const auto& m : j.o) select(m.second, st, i, out);
            else if (j.t == J::T::Arr) for (const J& e : j.a) select(e, st, i, out);
            break;
        }
    }

    // Modify at a wildcard-free path. mode: 0 set, 1 insert (only if missing), 2 replace (only if present). Returns true if changed.
    inline bool assign(J& root, const std::vector<Step>& st, const J& val, int mode) {
        if (st.empty()) { if (mode == 1) return false; root = val; return true; }
        J* cur = &root;
        for (size_t i = 0; i + 1 < st.size(); ++i) {
            const Step& s = st[i];
            J* nx = nullptr;
            if (s.k == Step::K::Key && cur->t == J::T::Obj) { for (auto& m : cur->o) if (m.first == s.key) nx = &m.second; }
            else if (s.k == Step::K::Idx && cur->t == J::T::Arr) { if (s.idx >= 0 && static_cast<size_t>(s.idx) < cur->a.size()) nx = &cur->a[static_cast<size_t>(s.idx)]; }
            else if (s.k == Step::K::Idx && s.idx == 0) nx = cur;
            if (!nx) return false;
            cur = nx;
        }
        const Step& f = st.back();
        if (f.k == Step::K::Key) {
            if (cur->t != J::T::Obj) return false;
            bool has = false;
            for (auto& m : cur->o) if (m.first == f.key) has = true;
            if ((has && mode == 1) || (!has && mode == 2)) return false;
            objSet(*cur, f.key, val);
            return true;
        }
        if (f.k == Step::K::Idx) {
            if (cur->t != J::T::Arr) {
                if (f.idx != 0) { if (mode == 2) return false; J w = J::arr(); w.a.push_back(*cur); w.a.push_back(val); *cur = std::move(w); return true; }
                if (mode == 1) return false;
                *cur = val;
                return true;
            }
            size_t n = static_cast<size_t>(f.idx);
            if (f.idx >= 0 && n < cur->a.size()) { if (mode == 1) return false; cur->a[n] = val; return true; }
            if (mode == 2) return false;
            cur->a.push_back(val);
            return true;
        }
        return false;
    }

    inline bool removeAt(J& root, const std::vector<Step>& st) {
        if (st.empty()) return false;
        J* cur = &root;
        for (size_t i = 0; i + 1 < st.size(); ++i) {
            const Step& s = st[i];
            J* nx = nullptr;
            if (s.k == Step::K::Key && cur->t == J::T::Obj) { for (auto& m : cur->o) if (m.first == s.key) nx = &m.second; }
            else if (s.k == Step::K::Idx && cur->t == J::T::Arr && s.idx >= 0 && static_cast<size_t>(s.idx) < cur->a.size()) nx = &cur->a[static_cast<size_t>(s.idx)];
            if (!nx) return false;
            cur = nx;
        }
        const Step& f = st.back();
        if (f.k == Step::K::Key && cur->t == J::T::Obj) {
            for (size_t k = 0; k < cur->o.size(); ++k) if (cur->o[k].first == f.key) { cur->o.erase(cur->o.begin() + static_cast<std::ptrdiff_t>(k)); return true; }
        }
        else if (f.k == Step::K::Idx && cur->t == J::T::Arr && f.idx >= 0 && static_cast<size_t>(f.idx) < cur->a.size()) {
            cur->a.erase(cur->a.begin() + f.idx);
            return true;
        }
        return false;
    }

    inline bool equalJ(const J& x, const J& y) {
        auto num = [](const J& j) { return j.t == J::T::Int || j.t == J::T::Real; };
        if (num(x) && num(y)) return (x.t == J::T::Int ? static_cast<double>(x.i) : x.d) == (y.t == J::T::Int ? static_cast<double>(y.i) : y.d);
        if (x.t != y.t) return false;
        switch (x.t) {
        case J::T::Null: return true;
        case J::T::Bool: return x.b == y.b;
        case J::T::Str: return x.s == y.s;
        case J::T::Arr:
            if (x.a.size() != y.a.size()) return false;
            for (size_t k = 0; k < x.a.size(); ++k) if (!equalJ(x.a[k], y.a[k])) return false;
            return true;
        case J::T::Obj:
            if (x.o.size() != y.o.size()) return false;
            for (size_t k = 0; k < x.o.size(); ++k) if (x.o[k].first != y.o[k].first || !equalJ(x.o[k].second, y.o[k].second)) return false;
            return true;
        default: return false;
        }
    }

    // JSON_CONTAINS semantics
    inline bool contains(const J& target, const J& cand) {
        if (target.t == J::T::Arr) {
            if (cand.t == J::T::Arr) {
                for (const J& c : cand.a) {
                    bool hit = false;
                    for (const J& t : target.a) if (contains(t, c)) { hit = true; break; }
                    if (!hit) return false;
                }
                return true;
            }
            for (const J& t : target.a) if (contains(t, cand)) return true;
            return false;
        }
        if (target.t == J::T::Obj) {
            if (cand.t != J::T::Obj) return false;
            for (const auto& cm : cand.o) {
                bool hit = false;
                for (const auto& tm : target.o) if (tm.first == cm.first && contains(tm.second, cm.second)) { hit = true; break; }
                if (!hit) return false;
            }
            return true;
        }
        return equalJ(target, cand);
    }

}  // namespace jsn
