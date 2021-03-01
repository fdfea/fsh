CXX = g++
CXXFLAGS = -Wall -O3 -std=c++17

all: fsh.exe

fsh.exe: fsh.o
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f *.o *.exe

.PHONY: clean all

