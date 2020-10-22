CXX = g++
CXXFLAGS = -g -std=c++11 -pedantic

all: miProxy

miProxy: miProxy.cpp tcp_helper.hpp
	$(CXX) $(CXXFLAGS) miProxy.cpp -o miProxy

clean:
	rm -rf miProxy *.dSYM

.PHONY: clean
