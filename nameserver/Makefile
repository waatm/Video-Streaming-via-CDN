CXX = g++
CXXFLAGS = -g -std=c++11 -pedantic

all: nameserver

miProxy: nameserver.cpp
	$(CXX) $(CXXFLAGS) nameserver.cpp -o nameserver

clean:
	rm -rf nameserver *.dSYM

.PHONY: clean