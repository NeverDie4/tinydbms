#include "page_file.h"
#include "record_page.h"

#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) throw std::runtime_error("integration check at line " + std::to_string(at.line()));
}

// Own only test-created paths. PageFile objects are destroyed before this directory.
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-record-persistence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { check(std::filesystem::create_directory(path)); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct Fixture {
    std::filesystem::path path;
    std::unique_ptr<PageFile> file;
    RawPage page;
    PageId id;
    explicit Fixture(std::filesystem::path p) : path(std::move(p)) {
        auto created = PageFile::create(path); check(created.value.has_value());
        file = std::move(*created.value);
        auto allocated = file->allocate_page(); check(allocated.value.has_value());
        id = *allocated.value; check(id == 1);
        check(!RecordPage::initialize(page, id));
    }
    void reopen() {
        check(!file->close());
        auto opened = PageFile::open(path); check(opened.value.has_value());
        file = std::move(*opened.value);
        auto read = file->read_page(id); check(read.value.has_value());
        page = *read.value;
    }
    void persist() { check(!file->write_page(id, page)); reopen(); }
    RecordId insert(const TableMeta& meta, const std::vector<Value>& values) {
        auto result = RecordPage::insert_record(page, id, meta, values);
        check(result.value.has_value()); return *result.value;
    }
    void expect(RecordId rid, const TableMeta& meta, const std::vector<Value>& values) {
        auto result = RecordPage::get_record(page, id, meta, rid);
        check(result.value.has_value()); check(result.value->rid.value == rid.value);
        check(result.value->values.size() == values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            check(result.value->values[i].data == values[i].data);
        }
    }
    void invalid(RecordId rid, const TableMeta& meta) {
        auto result = RecordPage::get_record(page, id, meta, rid);
        check(result.error && result.error->kind == RecordPageErrorKind::kInvalidArgument);
    }
};

// Golden format offsets used only to inspect/inject disk bytes independently of writers.
constexpr std::size_t kSelfId = 8, kSlotCount = 12, kLiveCount = 14, kFreeUpper = 18;
constexpr std::size_t kGeneration = 4, kFlags = 6;
std::uint16_t u16(const RawPage& page, std::size_t offset) {
    return std::to_integer<std::uint8_t>(page.bytes[offset]) |
        (std::to_integer<std::uint8_t>(page.bytes[offset + 1]) << 8);
}

void persistence(const std::filesystem::path& directory) {
    Fixture a(directory / "table_0.dat"), b(directory / "table_1.dat");
    a.persist(); check(!SlottedPage::validate(a.page, a.id));
    TableMeta meta{0,"mixed",{{"i",Type::kInt},{"j",Type::kInt},{"s",Type::kVarchar}}};
    std::vector<Value> alice{{std::int32_t{-42}},{std::int32_t{123456}},
        {std::string{"Alice\xE4\xB8\xAD\xF0\x9F\x99\x82"}}};
    auto bob = alice; bob.back() = Value{std::string{"Bob"}};
    auto carol = alice; carol.back() = Value{std::string{"Carol"}};
    auto ra = a.insert(meta, alice), rb = a.insert(meta, bob), rc = a.insert(meta, carol);
    auto other = b.insert(meta, bob); check(other.value == ra.value);
    a.persist(); b.persist();
    a.expect(ra,meta,alice); a.expect(rb,meta,bob); a.expect(rc,meta,carol); b.expect(other,meta,bob);
    check(!RecordPage::erase_record(a.page,a.id,rb)); a.persist();
    a.invalid(rb,meta); a.expect(ra,meta,alice); a.expect(rc,meta,carol);
    check(!SlottedPage::validate(a.page,a.id));
    auto replacement = a.insert(meta,bob);
    auto old_parts = *RecordIdCodec::decode(rb).value;
    auto new_parts = *RecordIdCodec::decode(replacement).value;
    check(old_parts.slot_id == new_parts.slot_id && new_parts.generation == old_parts.generation + 1);
    a.persist(); a.invalid(rb,meta); a.expect(replacement,meta,bob);
    auto before = a.page.bytes;
    check(RecordPage::erase_record(a.page,a.id,rb).has_value()); check(before == a.page.bytes);
    for (auto rid : {ra,replacement,rc}) check(!RecordPage::erase_record(a.page,a.id,rid));
    a.persist(); check(u16(a.page,kSlotCount)==3 && u16(a.page,kLiveCount)==0);
    check(u16(a.page,kFreeUpper)==kPageSize); check(a.file->page_count()==2);
    b.reopen(); b.expect(other,meta,bob);

    // Raw free-list pages are separate from ordinary RecordPages, which are retained.
    auto extra = a.file->allocate_page(); check(extra.value.has_value());
    check(!a.file->free_page(*extra.value)); a.reopen();
    auto reused = a.file->allocate_page(); check(reused.value == extra.value);
    check(!SlottedPage::validate(a.page,a.id));

    // Deterministic pre-I/O rejection, not a claim of atomic mid-write recovery.
    auto disk_before = a.page.bytes;
    (void)a.insert(meta,alice); check(a.page.bytes != disk_before);
    check(!a.file->close());
    auto error = a.file->write_page(a.id,a.page);
    check(error && error->kind==PageFileErrorKind::kInvalidArgument);
    a.reopen(); check(a.page.bytes==disk_before);
}

void corruption(const std::filesystem::path& directory) {
    Fixture f(directory / "corrupt.dat");
    TableMeta meta{0,"payload",{{"i",Type::kInt},{"s",Type::kVarchar}}};
    auto rid = f.insert(meta,{Value{std::int32_t{1}},Value{std::string{"abc"}}});
    f.persist(); const RawPage good = f.page;
    for (int case_id=0; case_id<4; ++case_id) {
        f.page=good;
        switch(case_id) {
            case 0: f.page.bytes[0]=std::byte{0}; break;
            case 1: f.page.bytes[kSelfId]=std::byte{2}; break;
            case 2: f.page.bytes[kSlottedPageHeaderSize+kFlags]=std::byte{0x80}; break;
            case 3: f.page.bytes[kSlottedPageHeaderSize+kGeneration]=std::byte{0}; break;
        }
        f.persist(); // PageFile format remains valid; record validation detects damage.
        auto result=RecordPage::get_record(f.page,f.id,meta,rid);
        check(result.error && result.error->kind==RecordPageErrorKind::kCorrupt);
    }
    f.page=good;
    f.page.bytes[kSlottedPageHeaderSize+kGeneration]=std::byte{2};
    f.persist(); f.invalid(rid,meta); // Nonzero generation mismatch is stale, not corrupt.
}
} // namespace

int main() {
    try { TempDirectory directory; persistence(directory.path); corruption(directory.path); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
