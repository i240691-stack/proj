CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
# Uncomment the one LIBS line that matches your GUI choice:
# LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt
# LIBS = $(shell sdl2-config --libs) -lrt
# LIBS = -lglfw -lGL -lrt
LIBS = -lncurses -lrt
TARGETS = arbiter hip asp
all: clean $(TARGETS)
	@echo Build complete.
arbiter: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)
hip: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)
asp: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)
clean:
	rm -f $(TARGETS)
.PHONY: all clean
