SOURCES=$(wildcard *.cpp)
default: ${SOURCES}
	g++ --std=c++20 -g ${SOURCES} -o bin/iomgr
