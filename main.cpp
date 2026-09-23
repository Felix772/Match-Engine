#include "orderbook.h"
#include "pipeline.h"
#include "itch.h"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
int main(int argc, char **argv) {
  try {
    if (argc == 1) process_csv_file("data.csv", true);
    else if (argc == 3 && std::string_view(argv[1]) == "--csv") process_csv_file(argv[2], true);
    else if (argc == 3 && std::string_view(argv[1]) == "--pipeline") run_csv_pipeline(argv[2], print_trade);
    else if (argc == 4 && std::string_view(argv[1]) == "--itch") {
      std::uint16_t locate{};
      std::string_view arg(argv[3]);
      auto [end, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), locate);
      if (ec != std::errc{} || end != arg.data() + arg.size() || !locate)
        throw std::invalid_argument("Locate must be in [1,65535]");
      std::cout << "Applied " << itch::replay(argv[2], locate) << " depth events\n";
    } else throw std::invalid_argument("Usage: match-engine [--csv FILE | --pipeline FILE | --itch FILE LOCATE]");
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
