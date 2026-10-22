#include "storage.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>

namespace {
    void wstr(std::ostream& o, const std::string& s) { o << s.size() << ':' << s << ' '; }
    std::string rstr(std::istream& i) {
        size_t n = 0;
        char c = 0;
        i >> n >> c;
        std::string s(n, '\0');
        i.read(&s[0], static_cast<std::streamsize>(n));
        return s;
    }
    void wval(std::ostream& o, const Value& v) {
        switch (v.t) {
        case Value::T::Null: o << "N "; break;
        case Value::T::Int: o << "I " << v.i << ' '; break;
        case Value::T::Real:
            if (v.scale >= 0) o << "D " << v.scale << ' ' << std::setprecision(17) << v.d << ' ';
            else o << "R " << std::setprecision(17) << v.d << ' ';
            break;
        case Value::T::Text: o << "T "; wstr(o, v.s); break;
        }
    }
    Value rval(std::istream& i) {
        char t = 0;
        i >> t;
        if (t == 'I') { int64_t x = 0; i >> x; return Value::integer(x); }
        if (t == 'R') { double d = 0; i >> d; return Value::real(d); }
        if (t == 'D') { int sc = 0; double d = 0; i >> sc >> d; Value x = Value::real(d); x.scale = sc; return x; }
        if (t == 'T') return Value::text(rstr(i));
        return Value();
    }
}  // namespace

void Catalog::save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("cannot write " + tmp);
        o << "MYSQLCLONE 4 " << lsn << ' ' << dbs.size() << ' ';
        for (const auto& d : dbs) {
            wstr(o, d.first);
            o << d.second.tables.size() << ' ';
            for (const auto& tp : d.second.tables) {
                const Table& t = tp.second;
                wstr(o, t.name);
                o << t.cols.size() << ' ';
                for (const Column& c : t.cols) {
                    wstr(o, c.name); wstr(o, c.typeName);
                    o << static_cast<int>(c.type) << ' ' << c.len << ' ' << c.notNull << ' ' << c.autoInc << ' '
                        << c.hasDef << ' ' << c.defNow << ' ';
                    wval(o, c.def);
                }
                o << t.uniques.size() << ' ';
                for (const UniqueKey& u : t.uniques) {
                    wstr(o, u.name);
                    o << u.cols.size() << ' ';
                    for (size_t x : u.cols) o << x << ' ';
                }
                o << t.keys.size() << ' ';
                for (const UniqueKey& u : t.keys) {
                    wstr(o, u.name);
                    o << u.cols.size() << ' ';
                    for (size_t x : u.cols) o << x << ' ';
                }
                o << t.autoNext << ' ' << t.rows.size() << ' ';
                for (const Row& r : t.rows) for (const Value& v : r) wval(o, v);
            }
            o << d.second.views.size() << ' ';
            for (const auto& vp : d.second.views) {
                wstr(o, vp.first);
                wstr(o, vp.second.sql);
                o << vp.second.cols.size() << ' ';
                for (const std::string& c : vp.second.cols) wstr(o, c);
            }
        }
        o.flush();
        if (!o) throw std::runtime_error("write failed: " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("rename failed for " + path);
}

bool Catalog::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string magic;
    int ver = 0;
    size_t nd = 0;
    in >> magic >> ver;
    lsn = 0;
    if (ver >= 2) in >> lsn;
    in >> nd;
    if (magic != "MYSQLCLONE" || (ver < 1 || ver > 4)) throw std::runtime_error("bad data file: " + path);
    dbs.clear();
    for (size_t di = 0; di < nd; ++di) {
        Database d;
        d.name = rstr(in);
        size_t nt = 0;
        in >> nt;
        for (size_t ti = 0; ti < nt; ++ti) {
            Table t;
            t.name = rstr(in);
            size_t nc = 0;
            in >> nc;
            for (size_t ci = 0; ci < nc; ++ci) {
                Column c;
                int type = 0;
                c.name = rstr(in); c.typeName = rstr(in);
                in >> type >> c.len >> c.notNull >> c.autoInc >> c.hasDef >> c.defNow;
                c.type = static_cast<CType>(type);
                c.def = rval(in);
                t.cols.push_back(std::move(c));
            }
            size_t nu = 0;
            in >> nu;
            for (size_t ui = 0; ui < nu; ++ui) {
                UniqueKey u;
                u.name = rstr(in);
                size_t k = 0;
                in >> k;
                for (size_t x = 0; x < k; ++x) { size_t c = 0; in >> c; u.cols.push_back(c); }
                t.uniques.push_back(std::move(u));
            }
            if (ver >= 3) {
                size_t nk = 0;
                in >> nk;
                for (size_t ui = 0; ui < nk; ++ui) {
                    UniqueKey u;
                    u.name = rstr(in);
                    size_t k = 0;
                    in >> k;
                    for (size_t x = 0; x < k; ++x) { size_t c = 0; in >> c; u.cols.push_back(c); }
                    t.keys.push_back(std::move(u));
                }
            }
            size_t nr = 0;
            in >> t.autoNext >> nr;
            for (size_t r = 0; r < nr; ++r) {
                Row row;
                for (size_t c = 0; c < nc; ++c) row.push_back(rval(in));
                t.rows.push_back(std::move(row));
            }
            d.tables[t.name] = std::move(t);
        }
        if (ver >= 4) {
            size_t nv = 0;
            in >> nv;
            for (size_t vi = 0; vi < nv; ++vi) {
                std::string vn = rstr(in);
                ViewDef vd;
                vd.sql = rstr(in);
                size_t nc = 0;
                in >> nc;
                for (size_t c = 0; c < nc; ++c) vd.cols.push_back(rstr(in));
                d.views[vn] = std::move(vd);
            }
        }
        dbs[d.name] = std::move(d);
    }
    if (!in) throw std::runtime_error("corrupt data file: " + path);
    return true;
}