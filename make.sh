#!/usr/bin/env sh
set -eu
mkdir -p build
g++ -O3 -DNDEBUG -std=c++20 -pthread -Wall -Wextra -Wpedantic -o build/match-engine main.cpp orderbook.cpp pipeline.cpp itch.cpp
g++ -O2 -std=c++20 -pthread -Wall -Wextra -Wpedantic -o build/engine-tests tests.cpp orderbook.cpp pipeline.cpp itch.cpp
./build/engine-tests
