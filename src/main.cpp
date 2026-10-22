#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "executor.hpp"
#include "lexer.hpp"
#include "server.hpp"

namespace {
	size_t dispLen(const std::string& s) { size_t n = 0; for (char c : s) if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n; return n; }
	bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

	void printTable(const Result& r) {
		size_t nc = r.headers.size();
		std::vector<size_t> w(nc);
		std::vector<bool> num(nc, true);
		for (size_t c = 0; c < nc; ++c) w[c] = dispLen(r.headers[c]);
		for (const Row& row : r.rows)
			for (size_t c = 0; c < nc; ++c) {
				w[c] = std::max(w[c], dispLen(row[c].str()));
				if (!row[c].isNull() && !row[c].isNum()) num[c] = false;
			}
		auto sep = [&]() { std::string l = "+"; for (size_t c = 0; c < nc; ++c) l += std::string(w[c] + 2, '-') + "+"; std::cout << l << "\n"; };
		auto cell = [&](const std::string& s, size_t c, bool right) {
			std::string pad(w[c] - dispLen(s), ' ');
			std::cout << " " << (right ? pad + s : s + pad) << " |";
			};
		sep();
		std::cout << "|";
		for (size_t c = 0; c < nc; ++c) cell(r.headers[c], c, false);
		std::cout << "\n";
		sep();
		for (const Row& row : r.rows) {
			std::cout << "|";
			for (size_t c = 0; c < nc; ++c) cell(row[c].str(), c, num[c] && !row[c].isNull());
			std::cout << "\n";
		}
		sep();
	}

	struct Shell {
		Engine eng;
		bool verbose;
		explicit Shell(std::string f, bool v) : eng(std::move(f)), verbose(v) {}

		bool run(const std::string& sql) {
			auto t0 = std::chrono::steady_clock::now();
			try {
				Result r = eng.execute(sql);
				if (r.silent) return true;
				double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				char tm[32];
				std::snprintf(tm, sizeof tm, " (%.2f sec)", sec);
				if (r.isSet) {
					if (verbose) {
						if (r.rows.empty()) std::cout << "Empty set" << tm << "\n\n";
						else {
							printTable(r);
							std::cout << r.rows.size() << (r.rows.size() == 1 ? " row" : " rows") << " in set" << tm << "\n\n";
						}
					}
					else {
						for (size_t c = 0; c < r.headers.size(); ++c) std::cout << (c ? "\t" : "") << r.headers[c];
						std::cout << "\n";
						for (const Row& row : r.rows) {
							for (size_t c = 0; c < row.size(); ++c) std::cout << (c ? "\t" : "") << row[c].str();
							std::cout << "\n";
						}
					}
				}
				else if (verbose) {
					std::cout << r.msg << (r.timing ? tm : "") << "\n";
					if (!r.info.empty()) std::cout << r.info << "\n";
					std::cout << "\n";
				}
				return true;
			}
			catch (const SqlError& e) {
				std::cout.flush();
				std::cerr << "ERROR " << e.code << " (" << e.state << "): " << e.what() << "\n";
			}
			catch (const std::exception& e) {
				std::cout.flush();
				std::cerr << "ERROR: " << e.what() << "\n";
			}
			return false;
		}

		// Feeds text; executes every complete statement. Returns false on error when stopOnError.
		bool feed(std::string& buf, bool stopOnError) {
			for (;;) {
				size_t e = findStatementEnd(buf);
				if (e == std::string::npos) return true;
				std::string stmt = buf.substr(0, e);
				buf.erase(0, e + 1);
				if (!run(stmt) && stopOnError) return false;
			}
		}
	};
}  // namespace

int main(int argc, char** argv) {
	std::string file = "mysqlclone.data", exec, db;
	bool table = false, force = false, crash = false;
	ServerOptions srv;
	int port = 0;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "-e" && i + 1 < argc) exec = argv[++i];
		else if (a == "-t") table = true;
		else if (a == "-f") force = true;
		else if (a == "--listen" && i + 1 < argc) port = std::atoi(argv[++i]);
		else if (a == "--bind" && i + 1 < argc) srv.bind = argv[++i];
		else if (a == "--user" && i + 1 < argc) srv.user = argv[++i];
		else if (a == "--password" && i + 1 < argc) srv.password = argv[++i];
		else if (a == "--crash-test") crash = true;  // exit without checkpoint (simulates a crash)
		else if (a == "--memory") file.clear();
		else if (a == "--data" && i + 1 < argc) file = argv[++i];
		else if (a == "-D" && i + 1 < argc) db = argv[++i];
		else {
			std::cerr << "usage: mysqlclone [-e SQL] [-t] [-f] [-D db] [--listen PORT [--bind ADDR] [--user U --password P]] [--data FILE | --memory]\n";
			return a == "-h" || a == "--help" ? 0 : 2;
		}
	}
	bool tty = isatty(0) != 0;
	try {
		Shell sh(file, tty || table);
		if (port > 0) { srv.port = port; return serve(sh.eng, srv); }
		if (!db.empty() && !sh.run("USE `" + db + "`")) return 1;
		std::string buf;
		if (!exec.empty()) {
			buf = exec + "\n";
			if (!sh.feed(buf, true)) return 1;
			if (!blank(buf) && !sh.run(buf)) return 1;
			if (crash) { std::cout.flush(); std::_Exit(0); }
			return 0;
		}
		if (tty) std::cout << "MySQL clone monitor. Commands end with ;\nServer version: 8.0.0-mysqlclone\nType 'exit' or '\\q' to quit.\n\n";
		std::string line;
		auto prompt = [&]() { if (tty) { std::cout << (blank(buf) ? "mysql> " : "    -> ") << std::flush; } };
		prompt();
		while (std::getline(std::cin, line)) {
			std::string t = line;
			while (!t.empty() && (t.back() == ';' || t.back() == ' ' || t.back() == '\r')) t.pop_back();
			if (blank(buf) && (t == "exit" || t == "quit" || t == "\\q")) break;
			buf += line + "\n";
			if (!sh.feed(buf, !tty && !force)) return 1;
			if (blank(buf)) buf.clear();
			prompt();
		}
		if (!blank(buf) && !sh.run(buf) && !tty && !force) return 1;
		if (tty) std::cout << "Bye\n";
		if (crash) { std::cout.flush(); std::_Exit(0); }
	}
	catch (const std::exception& e) {
		std::cerr << "FATAL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}