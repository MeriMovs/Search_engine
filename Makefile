CXX      = g++
CXXFLAGS = -std=c++17 -O2 -pthread
LDFLAGS  = -lcurl -lgumbo

TARGETS  = crawler

all: $(TARGETS)

crawler: crawler.cpp
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f $(TARGETS)

.PHONY: all clean
