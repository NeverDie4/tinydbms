#include "page_file.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>

using namespace tinydbms::storage::internal;
void check(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("allocation-state line " + std::to_string(at.line()));
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-allocation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { check(std::filesystem::create_directory(path)); }
    ~Temp() { std::error_code e; std::filesystem::remove_all(path,e); }
};
int main() try {
    Temp dir;
    auto made = PageFile::create(dir.path / "pages.dat"); check(made.value.has_value());
    auto file = std::move(*made.value);
    check(file->allocate_page().value == 1);
    check(file->allocate_page().value == 2);
    check(file->page_allocation_state(1).value == PageAllocationState::kAllocated);
    check(!file->free_page(1));
    check(file->page_allocation_state(1).value == PageAllocationState::kFree);
    check(file->page_allocation_state(2).value == PageAllocationState::kAllocated);
    check(file->page_allocation_state(0).error->kind == PageFileErrorKind::kInvalidArgument);
    check(file->page_allocation_state(3).error->kind == PageFileErrorKind::kInvalidArgument);
    check(!file->close());
    auto reopened = PageFile::open(dir.path / "pages.dat"); check(reopened.value.has_value());
    file = std::move(*reopened.value);
    check(file->page_allocation_state(1).value == PageAllocationState::kFree);
    check(file->page_allocation_state(2).value == PageAllocationState::kAllocated);
    check(file->allocate_page().value == 1);
    check(file->page_allocation_state(1).value == PageAllocationState::kAllocated);
    check(!file->free_page(1));
    // Deliberate external corruption of a free node, including querying that node.
    { std::fstream damage(dir.path / "pages.dat",std::ios::binary|std::ios::in|std::ios::out);
      check(damage.is_open()); damage.seekp(kPageSize); damage.put('X'); damage.close(); check(!damage.fail()); }
    check(file->page_allocation_state(1).error->kind == PageFileErrorKind::kCorrupt);
    check(file->page_allocation_state(2).error->kind == PageFileErrorKind::kCorrupt);
    check(!file->close());
    check(file->page_allocation_state(1).error->kind == PageFileErrorKind::kInvalidArgument);
    check(PageFile::open(dir.path / "pages.dat").error->kind == PageFileErrorKind::kCorrupt);
    auto cycle=PageFile::create(dir.path / "cycle.dat");check(cycle.value.has_value());
    auto& cyclic=**cycle.value;check(cyclic.allocate_page().value==1);check(!cyclic.free_page(1));
    {std::fstream damage(dir.path / "cycle.dat",std::ios::binary|std::ios::in|std::ios::out);
        damage.seekp(kPageSize+4);damage.put(char{1});damage.close();check(!damage.fail());}
    auto bad_allocate=cyclic.allocate_page();
    check(bad_allocate.error && bad_allocate.error->kind==PageFileErrorKind::kCorrupt);
    std::cout << "allocation-state tests passed\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
