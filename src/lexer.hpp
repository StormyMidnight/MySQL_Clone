#pragma once
#include <string>
#include <vector>

enum class Tok { End, Ident, QIdent, Num, Str, Sym };

struct Token {
    Tok type = Tok::End;
    std::string text;
    size_t pos = 0, end = 0;
    bool isReal = false;
};

std::string upper(const std::string& s);
std::vector<Token> lex(const std::string& sql);
// Index of the ';' terminating the first statement (outside quotes/comments), or npos.
size_t findStatementEnd(const std::string& sql);
