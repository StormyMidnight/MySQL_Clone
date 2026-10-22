CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
SRC	 := $(wildcard src/*.cpp)

mysqlclone: $(SRC) $(wildcard src/*.hpp)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: mysqlclone
	sh tests/run.sh
	sh tests/recovery.sh
	python3 tests/wire_test.py
	python3 tests/index_fuzz.py

clean:
	rm -f mysqlclone mysqlclone.data
.PHONY: test clean
