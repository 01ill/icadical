CXX = /usr/bin/clang++
CXXFLAGS = -g -Wall -Wextra -O3 -std=c++11 -pthread -pedantic
CPPFLAGS = -Ibuild/include
# CXXFLAGS = -g -Wall -Wextra -O3 -std=c++11 -pthread -pedantic -fsanitize=address


build/icadical: src/icadical.cpp cadical/build/libcadical.a | build/include/cadical
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/icadical.cpp cadical/build/libcadical.a -o $@

# link the src directory of cadical to the build directory
# this way we can omit the "src" in the header imports
build/include/cadical: | build cadical
	mkdir -p build/include
	ln -s ../../cadical/src $@

cadical/build/libcadical.a: cadical
	cd cadical && ./configure && make

cadical:
	git clone https://github.com/arminbiere/cadical.git

build:
	mkdir -p $@

clean:
	rm -rf build
	rm -rf cadical

.PHONY: clean
