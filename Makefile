CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LIBS = -lncurses -lrt

TARGETS = bin/arbiter bin/hip bin/asp

all: clean $(TARGETS)
	@echo Build complete.

bin/arbiter: arbiter/arbiter.cpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o bin/arbiter $(LIBS)

bin/hip: hip/hip.cpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) hip/*.cpp -o bin/hip $(LIBS)

bin/asp: asp/asp.cpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) asp/*.cpp -o bin/asp $(LIBS)

clean:
	rm -rf bin

.PHONY: all clean