CXXFLAGS=-fPIC -ggdb -O2 -Wall -Wextra -Werror -I.. -std=c++20

all:

.PHONY: all

all: shm-record shm-replay

shm-record: record.cpp shm.cpp shm.h
	$(CXX) $(CXXFLAGS) -o $@ record.cpp shm.cpp

shm-replay: replay.cpp shm.cpp shm.h
	$(CXX) $(CXXFLAGS) -o $@ replay.cpp shm.cpp
