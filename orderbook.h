#pragma once
#include <string>

struct Order;

bool parseLine(const std::string &line, Order &o);
void processOrder(Order o);

// “macro” function (like Bencher’s play_game loop) that your app/main and
// benchmark can both call:
void process_csv_file(const char *path, bool should_print);

// reset state so each benchmark iteration starts clean
void resetBook();
