CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread -I.

# SFML libraries
LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

TARGETS = arbiter_app hip_app asp_app

all: clean $(TARGETS)
	@echo Build complete.

arbiter_app: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hip_app: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

asp_app: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f arbiter_app hip_app asp_app

.PHONY: all clean
