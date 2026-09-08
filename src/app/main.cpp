#include <iostream>
#include <string_view>

namespace {

void print_help(std::ostream& output) {
    output << "Usage: tinydbms [--help|--version]\n"
           << "\n"
           << "Educational DBMS development scaffold.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        const std::string_view argument{argv[1]};
        if (argument == "--version") {
            std::cout << "tinydbms " << TINYDBMS_VERSION << '\n';
            return 0;
        }
        if (argument == "--help") {
            print_help(std::cout);
            return 0;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        print_help(std::cerr);
        return 2;
    }

    print_help(std::cout);
    return 0;
}
