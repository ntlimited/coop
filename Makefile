SOURCE_MAIN=main.cpp
SOURCE_COOP=$(wildcard coop/*.cpp)
SOURCE_COOP_NETWORK=$(wildcard coop/network/*.cpp)
SOURCE_COOP_TIME=$(wildcard coop/time/*.cpp)

default: ${SOURCES}
	g++ -I. --std=c++20 -g ${SOURCE_MAIN} ${SOURCE_COOP} ${SOURCE_COOP_TIME} ${SOURCE_COOP_NETWORK} -o bin/iomgr
