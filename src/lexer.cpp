#include "lexer.hpp"

#include <cctype>

#include "value.hpp"

std::string upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

static SqlError syntaxAt(const std::string& q, size_t at) {
    return SqlError(1064, "42000",
        "You have an error in your SQL syntax; check the manual that corresponds to your "
        "MySQL server version for the right syntax to use near '" +
        q.substr(std::min(at, q.size()), 30) + "' at line 1");
}

static bool isIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
static bool isIdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
static bool isDig(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Returns index just past a comment starting at i, or i if there is none.
static size_t skipComment(const std::string& q, size_t i) {
    size_t n = q.size();
    if (q[i] == '#' || (q[i] == '-' && i + 1 < n && q[i + 1] == '-' &&
        (i + 2 >= n || std::isspace(static_cast<unsigned char>(q[i + 2]))))) {
        while (i < n && q[i] != '\n') ++i;
        return i;
    }
    if (q[i] == '/' && i + 1 < n && q[i + 1] == '*') {
        size_t e = q.find("*/", i + 2);
        if (e == std::string::npos) throw syntaxAt(q, i);
        return e + 2;
    }
    return i;
}

std::vector<Token> lex(const std::string& q) {
    std::vector<Token> out;
    size_t i = 0, n = q.size();
    while (i < n) {
        char c = q[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        size_t j = skipComment(q, i);
        if (j != i) { i = j; continue; }

        Token t;
        t.pos = i;
        if (isIdStart(c)) {
            size_t s = i;
            while (i < n && isIdChar(q[i])) ++i;
            t.type = Tok::Ident;
            t.text = q.substr(s, i - s);
        }
        else if (isDig(c) || (c == '.' && i + 1 < n && isDig(q[i + 1]))) {
            size_t s = i;
            while (i < n && isDig(q[i])) ++i;
            if (i < n && q[i] == '.' && !(i + 1 < n && isIdStart(q[i + 1]))) {
                t.isReal = true;
                ++i;
                while (i < n && isDig(q[i])) ++i;
            }
            if (i < n && (q[i] == 'e' || q[i] == 'E')) {
                size_t k = i + 1;
                if (k < n && (q[k] == '+' || q[k] == '-')) ++k;
                if (k < n && isDig(q[k])) {
                    t.isReal = true;
                    i = k;
                    while (i < n && isDig(q[i])) ++i;
                }
            }
            t.type = Tok::Num;
            t.text = q.substr(s, i - s);
        }
        else if (c == '\'' || c == '"') {
            char quote = c;
            ++i;
            std::string v;
            bool closed = false;
            while (i < n) {
                char d = q[i];
                if (d == '\\' && i + 1 < n) {
                    char e = q[i + 1];
                    switch (e) {
                    case 'n': v += '\n'; break;
                    case 't': v += '\t'; break;
                    case 'r': v += '\r'; break;
                    case '0': v += '\0'; break;
                    default: v += e; break;
                    }
                    i += 2;
                }
                else if (d == quote) {
                    if (i + 1 < n && q[i + 1] == quote) { v += quote; i += 2; }
                    else { ++i; closed = true; break; }
                }
                else { v += d; ++i; }
            }
            if (!closed) throw syntaxAt(q, t.pos);
            t.type = Tok::Str;
            t.text = v;
        }
        else if (c == '`') {
            ++i;
            std::string v;
            bool closed = false;
            while (i < n) {
                if (q[i] == '`') {
                    if (i + 1 < n && q[i + 1] == '`') { v += '`'; i += 2; }
                    else { ++i; closed = true; break; }
                }
                else v += q[i++];
            }
            if (!closed) throw syntaxAt(q, t.pos);
            t.type = Tok::QIdent;
            t.text = v;
        }
        else {
            static const char* two[] = { "<=>", "<=", ">=", "<>", "!=", "||", "&&", ":=" };
            bool matched = false;
            for (const char* op : two) {
                std::string o = op;
                if (q.compare(i, o.size(), o) == 0) { t.text = o; i += o.size(); matched = true; break; }
            }
            if (!matched) {
                if (std::string("()*,.;+-/%=<>!@?").find(c) == std::string::npos) throw syntaxAt(q, i);
                t.text = std::string(1, c);
                ++i;
            }
            t.type = Tok::Sym;
        }
        t.end = i;
        out.push_back(std::move(t));
    }
    Token e;
    e.pos = e.end = n;
    out.push_back(e);
    return out;
}

size_t findStatementEnd(const std::string& q) {
    size_t i = 0, n = q.size();
    while (i < n) {
        char c = q[i];
        if (c == '\'' || c == '"' || c == '`') {
            char quote = c;
            ++i;
            while (i < n) {
                if (q[i] == '\\' && quote != '`' && i + 1 < n) { i += 2; continue; }
                if (q[i] == quote) {
                    if (i + 1 < n && q[i + 1] == quote) { i += 2; continue; }
                    break;
                }
                ++i;
            }
            if (i >= n) return std::string::npos;  // unterminated: wait for more input
            ++i;
        }
        else if (c == '#' || (c == '-' && i + 1 < n && q[i + 1] == '-' &&
            (i + 2 >= n || std::isspace(static_cast<unsigned char>(q[i + 2]))))) {
            while (i < n && q[i] != '\n') ++i;
        }
        else if (c == '/' && i + 1 < n && q[i + 1] == '*') {
            size_t e = q.find("*/", i + 2);
            if (e == std::string::npos) return std::string::npos;
            i = e + 2;
        }
        else if (c == ';') {
            return i;
        }
        else {
            ++i;
        }
    }
    return std::string::npos;
}
