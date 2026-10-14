# No vsomeip, no CMake dependency - deliberately standalone (see sender.cpp's header
# comment for why). Build: `make` in this directory. Run: see README.md.
CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -pthread

all: sender receiver

sender: sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f sender receiver

.PHONY: all clean
