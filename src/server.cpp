#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <random>

#include "lexer.hpp"
#include "parser.hpp"

namespace {
    volatile std::sig_atomic_t g_stop = 0;
    void onSignal(int) { g_stop = 1; }

    uint32_t u8(char c) { return static_cast<uint8_t>(c); }

    std::string sha1(const std::string& msg) {
        uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        std::string m = msg;
        uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
        m += static_cast<char>(0x80);
        while (m.size() % 64 != 56) m += '\0';
        for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (i * 8)) & 0xff);
        auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
        for (size_t off = 0; off < m.size(); off += 64) {
            uint32_t w[80];
            for (size_t i = 0; i < 16; ++i)
                w[i] = (u8(m[off + 4 * i]) << 24) | (u8(m[off + 4 * i + 1]) << 16) | (u8(m[off + 4 * i + 2]) << 8) | u8(m[off + 4 * i + 3]);
            for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        std::string out(20, '\0');
        for (size_t i = 0; i < 5; ++i)
            for (size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>((h[i] >> (24 - 8 * j)) & 0xff);
        return out;
    }

    // mysql_native_password: token = SHA1(pw) XOR SHA1(scramble + SHA1(SHA1(pw)))
    bool verifyPassword(const std::string& pw, const std::string& scramble, const std::string& token) {
        if (pw.empty()) return token.empty();
        std::string s1 = sha1(pw), mix = sha1(scramble + sha1(s1));
        for (size_t i = 0; i < 20; ++i) mix[i] = static_cast<char>(mix[i] ^ s1[i]);
        return token == mix;
    }

    void put16(std::string& s, uint32_t v) { s += static_cast<char>(v & 0xff); s += static_cast<char>((v >> 8) & 0xff); }
    void put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }
    std::string lenenc(uint64_t n) {
        std::string s;
        if (n < 251) s += static_cast<char>(n);
        else if (n < 65536) { s += '\xfc'; put16(s, static_cast<uint32_t>(n)); }
        else if (n < 16777216) { s += '\xfd'; put16(s, static_cast<uint32_t>(n & 0xffff)); s += static_cast<char>((n >> 16) & 0xff); }
        else { s += '\xfe'; put32(s, static_cast<uint32_t>(n & 0xffffffffu)); put32(s, static_cast<uint32_t>(n >> 32)); }
        return s;
    }
    std::string lstr(const std::string& s) { return lenenc(s.size()) + s; }

    std::string okPacket(int64_t affected, int64_t lastId, uint32_t status, const std::string& info) {
        std::string p(1, '\0');
        p += lenenc(static_cast<uint64_t>(affected));
        p += lenenc(static_cast<uint64_t>(lastId));
        put16(p, status);
        put16(p, 0);
        return p + info;
    }
    std::string errPacket(int code, const std::string& state, const std::string& msg) {
        std::string p(1, '\xff');
        put16(p, static_cast<uint32_t>(code));
        p += '#';
        p += (state + "00000").substr(0, 5);
        return p + msg;
    }
    std::string eofPacket(uint32_t status) {
        std::string p(1, '\xfe');
        put16(p, 0);
        put16(p, status);
        return p;
    }

    struct Conn {
        int fd = -1;
        uint8_t seq = 0;
        std::string wbuf;

        bool readN(char* b, size_t n) {
            while (n > 0) {
                ssize_t r = ::recv(fd, b, n, 0);
                if (r <= 0) return false;
                b += r;
                n -= static_cast<size_t>(r);
            }
            return true;
        }
        bool readPacket(std::string& out) {
            out.clear();
            for (;;) {
                char h[4];
                if (!readN(h, 4)) return false;
                size_t len = u8(h[0]) | (u8(h[1]) << 8) | (u8(h[2]) << 16);
                seq = static_cast<uint8_t>(u8(h[3]) + 1);
                size_t old = out.size();
                out.resize(old + len);
                if (len && !readN(&out[old], len)) return false;
                if (len < 0xFFFFFF) return true;
            }
        }
        void packet(const std::string& p) {
            size_t off = 0;
            bool more = true;
            while (more) {
                size_t n = std::min<size_t>(p.size() - off, 0xFFFFFF);
                wbuf += static_cast<char>(n & 0xff);
                wbuf += static_cast<char>((n >> 8) & 0xff);
                wbuf += static_cast<char>((n >> 16) & 0xff);
                wbuf += static_cast<char>(seq++);
                wbuf.append(p, off, n);
                off += n;
                more = n == 0xFFFFFF;
            }
        }
        bool flush() {
            size_t off = 0;
            while (off < wbuf.size()) {
                ssize_t r = ::send(fd, wbuf.data() + off, wbuf.size() - off, MSG_NOSIGNAL);
                if (r <= 0) { wbuf.clear(); return false; }
                off += static_cast<size_t>(r);
            }
            wbuf.clear();
            return true;
        }
    };

    std::string handshake(uint32_t id, const std::string& scramble) {
        std::string p(1, '\x0a');
        p += "8.0.0-mysqlclone";
        p += '\0';
        put32(p, id);
        p += scramble.substr(0, 8);
        p += '\0';
        put16(p, 0xA20D);  // LONG_PASSWORD|LONG_FLAG|CONNECT_WITH_DB|PROTOCOL_41|TRANSACTIONS|SECURE_CONNECTION
        p += static_cast<char>(45);  // utf8mb4_general_ci
        put16(p, 0x0002);            // autocommit
        put16(p, 0x0008);            // PLUGIN_AUTH
        p += static_cast<char>(21);
        p.append(10, '\0');
        p += scramble.substr(8, 12);
        p += '\0';
        p += "mysql_native_password";
        p += '\0';
        return p;
    }

    uint32_t statusOf(const Engine& e) { return 0x0002u | (e.inTransaction() ? 0x0001u : 0u); }

    struct ColMeta { uint8_t type = 0xfd; uint32_t charset = 45, length = 1020, flags = 0; uint8_t decimals = 0; };

    // Wire type of a result column, chosen from all of its values (INT -> LONGLONG, DECIMAL -> NEWDECIMAL, DOUBLE, else VAR_STRING).
    ColMeta colMeta(const Result& r, size_t i) {
        bool anyInt = false, anyReal = false, anyText = false, allScaled = true;
        int maxScale = 0;
        for (const Row& row : r.rows) {
            const Value& v = row[i];
            if (v.t == Value::T::Int) anyInt = true;
            else if (v.t == Value::T::Text) anyText = true;
            else if (v.t == Value::T::Real) {
                anyReal = true;
                if (v.scale < 0) allScaled = false; else maxScale = std::max(maxScale, v.scale);
            }
        }
        ColMeta m;
        if (anyText) return m;
        if (anyReal && allScaled) { m.type = 0xf6; m.charset = 63; m.length = 22; m.flags = 0x80; m.decimals = static_cast<uint8_t>(maxScale); }
        else if (anyReal) { m.type = 0x05; m.charset = 63; m.length = 22; m.flags = 0x80; m.decimals = 31; }
        else if (anyInt) { m.type = 0x08; m.charset = 63; m.length = 20; m.flags = 0x80; }
        return m;
    }

    std::string colDef(const std::string& name, const ColMeta& m) {
        std::string d = lstr("def") + lstr("") + lstr("") + lstr("") + lstr(name) + lstr(name);
        d += '\x0c';
        put16(d, m.charset);
        put32(d, m.length);
        d += static_cast<char>(m.type);
        put16(d, m.flags);
        d += static_cast<char>(m.decimals);
        put16(d, 0);
        return d;
    }

    void put64(std::string& s, uint64_t v) { put32(s, static_cast<uint32_t>(v & 0xffffffffu)); put32(s, static_cast<uint32_t>(v >> 32)); }

    // Text protocol rows (COM_QUERY) or binary protocol rows (COM_STMT_EXECUTE).
    void sendResultSet(Conn& c, const Result& r, uint32_t status, bool binary) {
        size_t nc = r.headers.size();
        std::vector<ColMeta> meta;
        for (size_t i = 0; i < nc; ++i) meta.push_back(colMeta(r, i));
        c.packet(lenenc(nc));
        for (size_t i = 0; i < nc; ++i) c.packet(colDef(r.headers[i], meta[i]));
        c.packet(eofPacket(status));
        for (const Row& row : r.rows) {
            std::string p;
            if (!binary) {
                for (const Value& v : row) p += v.isNull() ? std::string(1, '\xfb') : lstr(v.str());
            }
            else {
                p += '\0';
                std::string bm((nc + 9) / 8, '\0');
                for (size_t i = 0; i < nc; ++i)
                    if (row[i].isNull()) bm[(i + 2) / 8] = static_cast<char>(bm[(i + 2) / 8] | (1 << ((i + 2) % 8)));
                p += bm;
                for (size_t i = 0; i < nc; ++i) {
                    if (row[i].isNull()) continue;
                    if (meta[i].type == 0x08) put64(p, static_cast<uint64_t>(row[i].asInt()));
                    else if (meta[i].type == 0x05) { double d = row[i].asReal(); uint64_t u; std::memcpy(&u, &d, 8); put64(p, u); }
                    else p += lstr(row[i].str());
                }
            }
            c.packet(p);
        }
        c.packet(eofPacket(status));
    }

    bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

    void handleQuery(Engine& eng, Conn& c, const std::string& sql, bool binary = false) {
        std::string buf = sql;
        Result last;
        bool any = false;
        try {
            while (!blank(buf)) {
                size_t e = findStatementEnd(buf);
                std::string stmt = e == std::string::npos ? buf : buf.substr(0, e);
                buf = e == std::string::npos ? std::string() : buf.substr(e + 1);
                if (blank(stmt)) continue;
                last = eng.execute(stmt);
                any = true;
            }
        }
        catch (const SqlError& e) {
            c.packet(errPacket(e.code, e.state, e.what()));
            return;
        }
        catch (const std::exception& e) {
            c.packet(errPacket(1105, "HY000", e.what()));
            return;
        }
        if (!any) { c.packet(errPacket(1065, "42000", "Query was empty")); return; }
        uint32_t st = statusOf(eng);
        if (last.isSet) { sendResultSet(c, last, st, binary); return; }
        long long n = 0;
        if (!last.silent) std::sscanf(last.msg.c_str(), "Query OK, %lld", &n);
        c.packet(okPacket(n, eng.lastInsertId(), st, last.info));
    }

    // ---- prepared statements (binary protocol). Parameters are bound by rendering them as SQL literals. ----
    struct PStmt {
        std::string sql;
        size_t nParams = 0;
        std::vector<uint16_t> types;
        std::map<size_t, std::string> longData;
    };

    struct Reader {
        const std::string& b;
        size_t pos;
        bool ok = true;
        Reader(const std::string& s, size_t p) : b(s), pos(p) {}
        uint64_t num(size_t n) {
            if (pos + n > b.size()) { ok = false; return 0; }
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(u8(b[pos + i])) << (8 * i);
            pos += n;
            return v;
        }
        std::string bytes(size_t n) {
            if (pos + n > b.size()) { ok = false; return {}; }
            std::string s = b.substr(pos, n);
            pos += n;
            return s;
        }
        std::string lenstr() {
            uint64_t n = num(1);
            if (n == 0xfc) n = num(2); else if (n == 0xfd) n = num(3); else if (n == 0xfe) n = num(8);
            return bytes(static_cast<size_t>(n));
        }
    };

    std::string literal(const Value& v) {
        switch (v.t) {
        case Value::T::Null: return "NULL";
        case Value::T::Int: return v.i < 0 ? "(" + std::to_string(v.i) + ")" : std::to_string(v.i);
        case Value::T::Real: {
            if (!std::isfinite(v.d)) return "NULL";
            char b[64];
            if (v.scale >= 0) std::snprintf(b, sizeof b, "%.*f", v.scale, v.d);
            else std::snprintf(b, sizeof b, "%.17g", v.d);
            return v.d < 0 ? "(" + std::string(b) + ")" : std::string(b);
        }
        case Value::T::Text: {
            std::string r = "'";
            for (char ch : v.s) {
                if (ch == '\\' || ch == '\'') { r += '\\'; r += ch; }
                else if (ch == '\0') r += "\\0";
                else r += ch;
            }
            return r + "'";
        }
        }
        return "NULL";
    }

    // Replaces each '?' outside quotes/comments with the literal of the next parameter.
    std::string bindParams(const std::string& sql, const std::vector<Value>& vals) {
        std::string out;
        size_t k = 0, i = 0, n = sql.size();
        auto bad = [] { return SqlError(1210, "HY000", "Incorrect arguments to mysqld_stmt_execute"); };
        while (i < n) {
            char ch = sql[i];
            if (ch == '\'' || ch == '"' || ch == '`') {
                size_t st = i++;
                while (i < n) {
                    if (sql[i] == '\\' && ch != '`' && i + 1 < n) { i += 2; continue; }
                    if (sql[i] == ch) { if (i + 1 < n && sql[i + 1] == ch) { i += 2; continue; } break; }
                    ++i;
                }
                i = std::min(i + 1, n);
                out.append(sql, st, i - st);
            }
            else if (ch == '#' || (ch == '-' && i + 2 < n && sql[i + 1] == '-' && std::isspace(static_cast<unsigned char>(sql[i + 2])))) {
                size_t st = i;
                while (i < n && sql[i] != '\n') ++i;
                out.append(sql, st, i - st);
            }
            else if (ch == '/' && i + 1 < n && sql[i + 1] == '*') {
                size_t e = sql.find("*/", i + 2);
                size_t end = e == std::string::npos ? n : e + 2;
                out.append(sql, i, end - i);
                i = end;
            }
            else if (ch == '?') {
                if (k >= vals.size()) throw bad();
                out += literal(vals[k++]);
                ++i;
            }
            else { out += ch; ++i; }
        }
        if (k != vals.size()) throw bad();
        return out;
    }

    Value decodeParam(Reader& rd, uint16_t typeWord) {
        uint8_t t = static_cast<uint8_t>(typeWord & 0xff);
        bool uns = (typeWord >> 8) & 0x80;
        auto sgn = [&](uint64_t v, int bits) -> Value {
            if (!uns) { int64_t x = static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); return Value::integer(x); }
            if (v > static_cast<uint64_t>(INT64_MAX)) return Value::real(static_cast<double>(v));
            return Value::integer(static_cast<int64_t>(v));
            };
        char buf[64];
        switch (t) {
        case 0x06: return Value();
        case 0x01: return sgn(rd.num(1), 8);
        case 0x02: case 0x0d: return sgn(rd.num(2), 16);
        case 0x03: case 0x09: return sgn(rd.num(4), 32);
        case 0x08: return sgn(rd.num(8), 64);
        case 0x04: { uint32_t u = static_cast<uint32_t>(rd.num(4)); float f; std::memcpy(&f, &u, 4); return Value::real(static_cast<double>(f)); }
        case 0x05: { uint64_t u = rd.num(8); double d; std::memcpy(&d, &u, 8); return Value::real(d); }
        case 0x07: case 0x0a: case 0x0c: {
            size_t len = static_cast<size_t>(rd.num(1));
            if (len == 0) return Value::text("0000-00-00 00:00:00");
            unsigned y = static_cast<unsigned>(rd.num(2)), mo = static_cast<unsigned>(rd.num(1)), d = static_cast<unsigned>(rd.num(1));
            unsigned h = 0, mi = 0, s = 0;
            if (len >= 7) { h = static_cast<unsigned>(rd.num(1)); mi = static_cast<unsigned>(rd.num(1)); s = static_cast<unsigned>(rd.num(1)); }
            if (len >= 11) rd.num(4);
            if (t == 0x0a) std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", y, mo, d);
            else std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", y, mo, d, h, mi, s);
            return Value::text(buf);
        }
        case 0x0b: {
            size_t len = static_cast<size_t>(rd.num(1));
            if (len == 0) return Value::text("00:00:00");
            bool neg = rd.num(1) != 0;
            unsigned days = static_cast<unsigned>(rd.num(4)), h = static_cast<unsigned>(rd.num(1)), mi = static_cast<unsigned>(rd.num(1)), s = static_cast<unsigned>(rd.num(1));
            if (len >= 12) rd.num(4);
            std::snprintf(buf, sizeof buf, "%s%02u:%02u:%02u", neg ? "-" : "", days * 24 + h, mi, s);
            return Value::text(buf);
        }
        default: {
            std::string str = rd.lenstr();
            if (t == 0xf6 || t == 0x00) {  // DECIMAL arrives as text
                size_t dot = str.find('.');
                int sc = dot == std::string::npos ? 0 : static_cast<int>(str.size() - dot - 1);
                return Value::dec(std::strtod(str.c_str(), nullptr), std::min(sc, 30));
            }
            return Value::text(str);
        }
        }
    }

    void handlePrepare(Engine& eng, Conn& c, std::map<uint32_t, PStmt>& stmts, uint32_t& nextId, const std::string& sql) {
        size_t np = 0;
        try { np = static_cast<size_t>(parseStatement(sql).nParams); }
        catch (const SqlError& e) { c.packet(errPacket(e.code, e.state, e.what())); return; }
        std::vector<std::string> cols = eng.resultColumns(bindParams(sql, std::vector<Value>(np)));
        uint32_t id = ++nextId;
        PStmt ps;
        ps.sql = sql;
        ps.nParams = np;
        stmts[id] = ps;
        uint32_t st = statusOf(eng);
        std::string ok(1, '\0');
        put32(ok, id);
        put16(ok, static_cast<uint32_t>(cols.size()));
        put16(ok, static_cast<uint32_t>(np));
        ok += '\0';
        put16(ok, 0);
        c.packet(ok);
        ColMeta dm;
        if (np > 0) { for (size_t i = 0; i < np; ++i) c.packet(colDef("?", dm)); c.packet(eofPacket(st)); }
        if (!cols.empty()) { for (const std::string& n : cols) c.packet(colDef(n, dm)); c.packet(eofPacket(st)); }
    }

    void handleExecute(Engine& eng, Conn& c, std::map<uint32_t, PStmt>& stmts, const std::string& arg) {
        Reader rd(arg, 0);
        uint32_t id = static_cast<uint32_t>(rd.num(4));
        rd.num(1);  // flags (cursors unsupported)
        rd.num(4);  // iteration count
        auto it = stmts.find(id);
        auto bad = [&] { c.packet(errPacket(1210, "HY000", "Incorrect arguments to mysqld_stmt_execute")); };
        if (!rd.ok || it == stmts.end()) {
            c.packet(errPacket(1243, "HY000", "Unknown prepared statement handler (" + std::to_string(id) + ") given to mysqld_stmt_execute"));
            return;
        }
        PStmt& ps = it->second;
        std::vector<Value> vals(ps.nParams);
        if (ps.nParams > 0) {
            std::string nullbm = rd.bytes((ps.nParams + 7) / 8);
            if (rd.num(1) != 0) {
                ps.types.assign(ps.nParams, 0);
                for (size_t i = 0; i < ps.nParams; ++i) ps.types[i] = static_cast<uint16_t>(rd.num(2));
            }
            if (!rd.ok || ps.types.size() != ps.nParams) { bad(); return; }
            for (size_t i = 0; i < ps.nParams; ++i) {
                if ((u8(nullbm[i / 8]) >> (i % 8)) & 1) continue;
                auto ld = ps.longData.find(i);
                if (ld != ps.longData.end()) { vals[i] = Value::text(ld->second); continue; }
                vals[i] = decodeParam(rd, ps.types[i]);
            }
            if (!rd.ok) { bad(); return; }
        }
        ps.longData.clear();
        std::string bound;
        try { bound = bindParams(ps.sql, vals); }
        catch (const SqlError& e) { c.packet(errPacket(e.code, e.state, e.what())); return; }
        handleQuery(eng, c, bound, true);
    }

    void serveClient(Engine& eng, int fd, const ServerOptions& opt, uint32_t id, const std::string& host) {
        Conn c;
        c.fd = fd;
        std::random_device rd;
        std::string scramble;
        for (int i = 0; i < 20; ++i) scramble += static_cast<char>(1 + rd() % 255);  // never NUL
        c.packet(handshake(id, scramble));
        std::string p;
        if (!c.flush() || !c.readPacket(p) || p.size() < 33) return;
        uint32_t caps = u8(p[0]) | (u8(p[1]) << 8) | (u8(p[2]) << 16) | (u8(p[3]) << 24);
        if (!(caps & 0x200)) { c.packet(errPacket(1043, "08S01", "Bad handshake")); c.flush(); return; }
        size_t pos = 32, z = p.find('\0', pos);
        if (z == std::string::npos) return;
        std::string user = p.substr(pos, z - pos), auth, db;
        pos = z + 1;
        if (pos < p.size()) {
            size_t n = 0;
            if (caps & (0x8000u | 0x200000u)) { n = u8(p[pos]); ++pos; }
            if (pos + n > p.size()) return;
            auth = p.substr(pos, n);
            pos += n;
        }
        if ((caps & 0x8) && pos < p.size()) {
            size_t z2 = p.find('\0', pos);
            db = p.substr(pos, (z2 == std::string::npos ? p.size() : z2) - pos);
        }
        bool ok = (opt.user.empty() && opt.password.empty()) ||
            ((opt.user.empty() || user == opt.user) && verifyPassword(opt.password, scramble, auth));
        if (!ok) {
            c.packet(errPacket(1045, "28000", "Access denied for user '" + user + "'@'" + host + "' (using password: " + (auth.empty() ? "NO" : "YES") + ")"));
            c.flush();
            return;
        }
        if (!db.empty()) {
            try { eng.execute("USE `" + db + "`"); }
            catch (const SqlError& e) { c.packet(errPacket(e.code, e.state, e.what())); c.flush(); return; }
        }
        c.packet(okPacket(0, 0, statusOf(eng), ""));
        if (!c.flush()) return;
        std::map<uint32_t, PStmt> stmts;
        uint32_t nextStmt = 0;
        for (;;) {
            if (!c.readPacket(p) || p.empty()) return;
            uint8_t cmd = static_cast<uint8_t>(p[0]);
            std::string arg = p.substr(1);
            if (cmd == 0x01) return;                                   // COM_QUIT
            else if (cmd == 0x0e) c.packet(okPacket(0, 0, statusOf(eng), ""));  // COM_PING
            else if (cmd == 0x02) handleQuery(eng, c, "USE `" + arg + "`");     // COM_INIT_DB
            else if (cmd == 0x03) handleQuery(eng, c, arg);            // COM_QUERY
            else if (cmd == 0x16) handlePrepare(eng, c, stmts, nextStmt, arg);  // COM_STMT_PREPARE
            else if (cmd == 0x17) handleExecute(eng, c, stmts, arg);            // COM_STMT_EXECUTE
            else if (cmd == 0x18) {                                              // COM_STMT_SEND_LONG_DATA (no reply)
                Reader rd(arg, 0);
                uint32_t sid = static_cast<uint32_t>(rd.num(4));
                size_t pid = static_cast<size_t>(rd.num(2));
                auto f = stmts.find(sid);
                if (rd.ok && f != stmts.end()) f->second.longData[pid] += arg.substr(rd.pos);
            }
            else if (cmd == 0x19) { Reader rd(arg, 0); stmts.erase(static_cast<uint32_t>(rd.num(4))); }  // COM_STMT_CLOSE (no reply)
            else if (cmd == 0x1a) {                                                                        // COM_STMT_RESET
                Reader rd(arg, 0);
                auto f = stmts.find(static_cast<uint32_t>(rd.num(4)));
                if (f != stmts.end()) f->second.longData.clear();
                c.packet(okPacket(0, 0, statusOf(eng), ""));
            }
            else c.packet(errPacket(1047, "08S01", "Unknown command"));
            if (!c.flush()) return;
        }
    }
}  // namespace

int serve(Engine& eng, const ServerOptions& opt) {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: blocking calls return EINTR so we can stop
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { std::cerr << "socket failed\n"; return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(opt.port));
    if (inet_pton(AF_INET, opt.bind.c_str(), &addr.sin_addr) != 1) { std::cerr << "bad bind address\n"; return 1; }
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 16) != 0) {
        std::cerr << "cannot listen on " << opt.bind << ":" << opt.port << "\n";
        ::close(ls);
        return 1;
    }
    std::cout << "mysqlclone listening on " << opt.bind << ":" << opt.port << std::endl;
    uint32_t id = 0;
    while (!g_stop) {
        sockaddr_in peer{};
        socklen_t pl = sizeof peer;
        int fd = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &pl);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        eng.resetSession();
        serveClient(eng, fd, opt, ++id, ip);
        ::close(fd);
        eng.resetSession();  // rolls back anything left uncommitted
    }
    ::close(ls);
    return 0;
}