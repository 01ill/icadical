CXX = /usr/bin/clang++
CXXFLAGS = -g -Wall -Wextra -std=c++11 -pthread -pedantic
CPPFLAGS = -Ibuild/include
# CXXFLAGS = -g -Wall -Wextra -O3 -std=c++11 -pthread -pedantic -fsanitize=address

all: build/icadical build/ccadical

build/icadical: src/icadical.cpp cadical/build/libcadical.a | build/include/cadical
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/icadical.cpp cadical/build/libcadical.a -o $@

build/ccadical: src/ccadical.cpp cadical/build/libcadical.a | build/include/cadical
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/ccadical.cpp cadical/build/libcadical.a -o $@

# link the src directory of cadical to the build directory
# this way we can omit the "src" in the header imports
build/include/cadical: | build cadical
	mkdir -p build/include
	ln -s ../../cadical/src $@

CADICAL_SRC = $(wildcard cadical/src/*.cpp cadical/src/*.hpp cadical/src/*.h)

cadical/build/makefile: makefile | cadical
	cd cadical && ./configure

# recompile when Cadical source changes
cadical/build/libcadical.a: $(CADICAL_SRC) cadical/build/makefile
	$(MAKE) -C cadical/build -j8 libcadical.a

cadical:
	git clone https://github.com/arminbiere/cadical.git

build:
	mkdir -p $@

clean:
	rm -rf build
	rm -rf cadical

.PHONY: clean
