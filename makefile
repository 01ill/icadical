CXX = /usr/bin/clang++
CXXFLAGS = -g -Wall -Wextra -O3 -std=c++11 -pthread -pedantic
# CXXFLAGS = -g -Wall -Wextra -O3 -std=c++11 -pthread -pedantic -fsanitize=address


build/icadical: icadical.cpp libcadical.a | build
	$(CXX) $(CXXFLAGS) icadical.cpp libcadical.a -o $@

build:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: clean
