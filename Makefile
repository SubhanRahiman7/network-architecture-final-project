# PBF/1 - server (pbfd), client (pbf) and the test program.
#
#   make          build ./pbfd and ./pbf
#   make test     build everything and run the tests
#   make interop  run the C++ programs against independent Python implementations of SPEC.md
#   make hexdump  regenerate HEXDUMP.md from a real exchange (hexdump-check: verify it is current)
#   make clean
#
# Linux/macOS: gcc or clang with C++17. Windows: MinGW-w64 g++ (adds -lws2_32).

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc
LDLIBS   =
EXE      =
ifeq ($(OS),Windows_NT)
  LDLIBS = -static -lws2_32   # static: the .exe must not depend on MinGW DLLs being on PATH
  EXE    = .exe
endif

HEADERS = src/net.hpp src/wire.hpp src/server.hpp src/client.hpp

all: pbfd$(EXE) pbf$(EXE)

pbfd$(EXE): src/pbfd.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ src/pbfd.cpp $(LDLIBS)

pbf$(EXE): src/pbf.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ src/pbf.cpp $(LDLIBS)

tests/test$(EXE): tests/test.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ tests/test.cpp $(LDLIBS)

test: all tests/test$(EXE)
	./tests/test$(EXE)

# two independent Python implementations, written from SPEC.md, against the C++ programs (both directions)
interop: all
	python3 tools/interop.py

# HEXDUMP.md is generated from the real programs; `hexdump-check` fails if it is stale
hexdump: all
	python3 tools/annotate.py

hexdump-check: all
	python3 tools/annotate.py --check

clean:
	rm -f pbfd pbf tests/test pbfd.exe pbf.exe tests/test.exe

.PHONY: all test interop hexdump hexdump-check clean
