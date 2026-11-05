#include "sdgo/unit_form_graph_format.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    auto read = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("unit form file unavailable");
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
    };
    try {
        const auto candidate = read(argv[1]), authority = read(argv[2]);
        sdgo::unit_forms::GraphRuntimeInput input;std::string error;
        if (!input.Parse(candidate, authority, error)) { std::cerr << error << '\n';return 1; }
        std::cout << "unit form candidate/independent authority parsed: " << input.candidates.size() << " owners; no native activation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
