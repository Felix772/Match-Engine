#include "orderbook.h"
#include <iostream>

int main() {
  try {
    process_csv_file("data.csv", true);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}
