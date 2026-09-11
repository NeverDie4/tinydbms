#include "tinydbms/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace tinydbms::storage;

void check(bool value) {
    if (!value) {
        throw std::runtime_error{"legacy metadata regression assertion"};
    }
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-v1-meta-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory() { check(std::filesystem::create_directory(path)); }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void rejects_v1_without_rewriting() {
    TemporaryDirectory directory;
    const std::string v1 =
        "TINYDBMS_STORAGE_V1\nTABLE 2 first\nCOLUMN INT32 id\nENDTABLE\nEND\n";
    {
        std::ofstream output{directory.path / "storage.meta"};
        output << v1;
        output.close();
        check(!output.fail());
    }

    const auto opened = open_storage({directory.path.string()});
    check(opened.error.has_value());
    check(opened.error->kind == StorageErrorKind::kInvalidRequest);
    std::ifstream input{directory.path / "storage.meta"};
    const std::string after{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    check(after == v1);
}

}  // namespace

int main() {
    try {
        rejects_v1_without_rewriting();
        std::cout << "legacy V1 metadata rejection regression passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
