#pragma once
#include <string>
#include <vector>

enum class FKAction { NoAction, Restrict, Cascade, SetNull };

struct FkDef {
    std::string name, refTable, refDb; // refDb is only used while parsing (cross-database FKs are unsupported)
    std::vector<std::string> cols, refCols;
    FkAction onDelete = FkAction::NoAction, onUpdate = FkAction::NoAction;
};
