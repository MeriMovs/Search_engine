CXX      = g++
CXXFLAGS = -std=c++17 -O2 -pthread
LDFLAGS  = -lcurl -lgumbo
GO       = go

all: crawler search-api/search-api

crawler: crawler.cpp
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

search-api/search-api: search-api/main.go search-api/go.mod
	cd search-api && $(GO) build -o search-api .

clean:
	rm -f crawler search-api/search-api

.PHONY: all clean
