SOURCE_MAIN=main.cpp
SOURCE_COOP=$(wildcard coop/*.cpp)
SOURCE_COOP_NETWORK=$(wildcard coop/network/*.cpp)
SOURCE_COOP_TIME=$(wildcard coop/time/*.cpp)
SOURCE_COOP_IO=$(wildcard coop/io/*.cpp)

SOURCE =${SOURCE_COOP}
SOURCE+=${SOURCE_COOP_NETWORK}
SOURCE+=${SOURCE_COOP_TIME}
SOURCE+=${SOURCE_COOP_IO}

CC=g++
CXXFLAGS=-I. --std=c++20 -g -O0 -Wcpp
LDFLAGS=-luring -lwolfssl

default: clean
	make -j16 bin/server bin/client

obj:
	mkdir obj obj/time obj/network

obj/%.o: %.cpp
	${CC} ${CXXFLAGS} -c $< -o $@

OBJECTS=$(patsubst %.cpp,obj/%.o,${SOURCE})

bin/server: server.cpp ${OBJECTS}
	${CC} ${CXXFLAGS} $^ -o $@ ${LDFLAGS}

bin/client: client.cpp ${OBJECTS}
	${CC} ${CXXFLAGS} $^ -o $@ ${LDFLAGS}

clean:
	rm -f bin/iomgr $$(find obj/ -name *.o)
