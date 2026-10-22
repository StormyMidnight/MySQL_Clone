#pragma once
#include <string>

#include "ast.hpp"

Stmt parseStatement(const std::string& sql);
