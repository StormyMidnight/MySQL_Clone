#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "value.hpp"

using Row = std::vector<Value>;

struct Column {
	std::string name, typeName;
	CType type = CType::Int;
	int len = 0;
	bool notNull = false, autoInc = false, hasDef = false, defNow = false;
	Value def;
};
struct UniqueKey { std::string name; std::vector<size_t> cols; };  // "PRIMARY" is always first if present

// Lexicographic order over compareValues; a shorter key sorts before any extension of it (prefix scans).
struct KeyLess {
	bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
		size_t n = std::min(a.size(), b.size());
		for (size_t i = 0; i < n; ++i) {
			int c = compareValues(a[i], b[i]);
			if (c != 0) return c < 0;
		}
		return a.size() < b.size();
	}
};
using IndexMap = std::map<std::vector<Value>, std::vector<size_t>, KeyLess>;

struct Table {
	std::string name;
	std::vector<Column> cols;
	std::vector<Row> rows;
	std::vector<UniqueKey> uniques;  // PRIMARY / UNIQUE keys
	std::vector<UniqueKey> keys;     // non-unique secondary indexes
	int64_t autoNext = 1;

	// Ordered in-memory indexes: uniques first, then keys. key -> row positions. Rows with a NULL
	// in an indexed column are not indexed. Derived data: rebuilt lazily after bulk changes.
	mutable std::vector<IndexMap> idx;
	mutable bool idxValid = false;

	int find(const std::string& c) const {
		for (size_t i = 0; i < cols.size(); ++i) if (ciCompare(cols[i].name, c) == 0) return static_cast<int>(i);
		return -1;
	}
	bool hasPk() const { return !uniques.empty() && uniques[0].name == "PRIMARY"; }

	size_t nIndexes() const { return uniques.size() + keys.size(); }
	const UniqueKey& indexDef(size_t i) const { return i < uniques.size() ? uniques[i] : keys[i - uniques.size()]; }
	bool makeKey(const UniqueKey& d, const Row& r, std::vector<Value>& k) const {
		k.clear();
		for (size_t c : d.cols) {
			if (r[c].isNull()) return false;
			k.push_back(r[c]);
		}
		return true;
	}
	void invalidate() const { idxValid = false; }
	void ensureIndexes() const {
		if (idxValid) return;
		idx.assign(nIndexes(), IndexMap());
		idxValid = true;
		for (size_t r = 0; r < rows.size(); ++r) indexAdd(rows[r], r);
	}
	void indexAdd(const Row& r, size_t ri) const {
		if (!idxValid) return;
		std::vector<Value> k;
		for (size_t i = 0; i < nIndexes(); ++i)
			if (makeKey(indexDef(i), r, k)) idx[i][k].push_back(ri);
	}
	void indexRemove(const Row& r, size_t ri) const {
		if (!idxValid) return;
		std::vector<Value> k;
		for (size_t i = 0; i < nIndexes(); ++i) {
			if (!makeKey(indexDef(i), r, k)) continue;
			auto it = idx[i].find(k);
			if (it == idx[i].end()) continue;
			auto& v = it->second;
			v.erase(std::remove(v.begin(), v.end(), ri), v.end());
			if (v.empty()) idx[i].erase(it);
		}
	}
};
struct ViewDef { std::string sql; std::vector<std::string> cols; };  // sql = the SELECT text
struct Database {
	std::string name;
	std::map<std::string, Table> tables;
	std::map<std::string, ViewDef> views;
};
struct Catalog {
	std::map<std::string, Database> dbs;
	uint64_t lsn = 0;  // last WAL group included in this snapshot
	void save(const std::string& path) const;  // atomic (tmp + rename)
	bool load(const std::string& path);        // false if file absent
};