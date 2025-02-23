SOURCE_MAIN=main.cpp
SOURCE_COOP=$(wildcard coop/*.cpp)
SOURCE_COOP_NETWORK=$(wildcard coop/network/*.cpp)
SOURCE_COOP_TIME=$(wildcard coop/time/*.cpp)

SOURCE =${SOURCE_COOP}
SOURCE+=${SOURCE_COOP_NETWORK}
SOURCE+=${SOURCE_COOP_TIME}

CC=g++
CXXFLAGS=-I. --std=c++20 -g

default: bin/iomgr

obj:
	mkdir obj obj/time obj/network

obj/%.o: %.cpp
	${CC} ${CXXFLAGS} -c $< -o $@

OBJECTS=$(patsubst %.cpp,obj/%.o,${SOURCE})

bin/iomgr: main.cpp ${OBJECTS}
	${CC} ${CXXFLAGS} $^ -o $@

clean:
	rm $$(find obj/ -name *.o)
	rm bin/iomgr
