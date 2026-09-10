#include "slotted_page.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {

using tinydbms::storage::internal::PageId;
using tinydbms::storage::internal::RawPage;
using tinydbms::storage::internal::SlotHandle;
using tinydbms::storage::internal::SlottedPage;
using tinydbms::storage::internal::SlottedPageError;
using tinydbms::storage::internal::SlottedPageErrorKind;
using tinydbms::storage::internal::SlottedPageResult;
using tinydbms::storage::internal::kMaxRecordPayloadBytes;
using tinydbms::storage::internal::kMaxSlotsPerPage;
using tinydbms::storage::internal::kPageSize;
using tinydbms::storage::internal::kSlotEntrySize;
using tinydbms::storage::internal::kSlotOccupied;
using tinydbms::storage::internal::kSlotRetired;
using tinydbms::storage::internal::kSlottedPageHeaderSize;

constexpr std::size_t kSlotCountOffset = 12;
constexpr std::size_t kLiveCountOffset = 14;
constexpr std::size_t kFreeLowerOffset = 16;
constexpr std::size_t kFreeUpperOffset = 18;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "expectation failed: " << message << '\n';
    }
    return condition;
}

template <typename T>
bool has_error(const SlottedPageResult<T>& result, SlottedPageErrorKind kind) {
    return !result.value.has_value() && result.error.has_value() && result.error->kind == kind;
}

bool has_error(const std::optional<SlottedPageError>& error, SlottedPageErrorKind kind) {
    return error.has_value() && error->kind == kind;
}

std::uint16_t read_u16(const RawPage& page, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(page.bytes[offset])) |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(page.bytes[offset + 1]) << 8U);
}

void write_u16(RawPage& page, std::size_t offset, std::uint16_t value) {
    page.bytes[offset] = std::byte{static_cast<std::uint8_t>(value & 0xffU)};
    page.bytes[offset + 1] = std::byte{static_cast<std::uint8_t>((value >> 8U) & 0xffU)};
}

void write_u32(RawPage& page, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page.bytes[offset + index] =
            std::byte{static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU)};
    }
}

std::size_t slot_offset(std::uint16_t slot_id) {
    return kSlottedPageHeaderSize + static_cast<std::size_t>(slot_id) * kSlotEntrySize;
}

std::vector<std::byte> payload(std::size_t size, std::uint8_t seed) {
    std::vector<std::byte> result(size);
    for (std::size_t index = 0; index < size; ++index) {
        result[index] = std::byte{static_cast<std::uint8_t>(seed + index)};
    }
    return result;
}

RawPage initialized(PageId page_id = 1) {
    RawPage page;
    const auto error = SlottedPage::initialize(page, page_id);
    if (error.has_value()) {
        std::cerr << "test setup failed to initialize SlottedPage\n";
    }
    return page;
}

bool test_initialize_layout() {
    RawPage page;
    page.bytes.fill(std::byte{0xff});
    bool passed = expect(!SlottedPage::initialize(page, 7).has_value(),
                         "nonzero PageId must initialize");
    passed = expect(page.bytes[0] == std::byte{'T'} && page.bytes[1] == std::byte{'S'} &&
                        page.bytes[2] == std::byte{'P'} && page.bytes[3] == std::byte{'1'},
                    "initialized magic must be TSP1") && passed;
    passed = expect(read_u16(page, 4) == 1 && read_u16(page, 6) == 32,
                    "initialized version/header_size must match frozen format") && passed;
    passed = expect(read_u16(page, kSlotCountOffset) == 0 &&
                        read_u16(page, kLiveCountOffset) == 0 &&
                        read_u16(page, kFreeLowerOffset) == 32 &&
                        read_u16(page, kFreeUpperOffset) == 4096,
                    "empty-page counters must match frozen format") && passed;
    passed = expect(std::all_of(page.bytes.begin() + 20, page.bytes.begin() + 32,
                                [](std::byte byte) { return byte == std::byte{0}; }),
                    "reserved header bytes must be zero") && passed;
    passed = expect(!SlottedPage::validate(page, 7).has_value(),
                    "new page must validate") && passed;
    passed = expect(has_error(SlottedPage::initialize(page, 0),
                              SlottedPageErrorKind::kInvalidArgument),
                    "PageId 0 must not initialize as RecordPage") && passed;
    passed = expect(kMaxSlotsPerPage == (kPageSize - kSlottedPageHeaderSize) / kSlotEntrySize,
                    "maximum slot count must use named layout constants") && passed;
    return passed;
}

bool test_insert_get_and_multiple_records() {
    RawPage page = initialized(3);
    const auto first_payload = payload(3, 10);
    const auto second_payload = payload(7, 30);
    const auto first = SlottedPage::insert(page, 3, first_payload);
    const auto second = SlottedPage::insert(page, 3, second_payload);
    bool passed = expect(first.value == std::optional<SlotHandle>{{0, 1}} &&
                             second.value == std::optional<SlotHandle>{{1, 1}},
                         "first inserts must allocate slot 0/1 generation 1");
    const auto first_read = SlottedPage::get(page, 3, *first.value);
    const auto second_read = SlottedPage::get(page, 3, *second.value);
    passed = expect(first_read.value == std::optional{first_payload} &&
                        second_read.value == std::optional{second_payload},
                    "get must return byte-perfect owned payloads") && passed;
    passed = expect(read_u16(page, kSlotCountOffset) == 2 &&
                        read_u16(page, kLiveCountOffset) == 2 &&
                        read_u16(page, kFreeLowerOffset) == 48 &&
                        read_u16(page, kFreeUpperOffset) == 4096 - 10,
                    "slots must grow upward and payloads downward") && passed;
    passed = expect(!SlottedPage::validate(page, 3).has_value(),
                    "multi-record page must validate") && passed;
    return passed;
}

bool test_delete_compact_and_delete_all() {
    RawPage page = initialized(4);
    const auto a = payload(5, 1);
    const auto b = payload(9, 20);
    const auto c = payload(4, 50);
    const auto slot_a = SlottedPage::insert(page, 4, a).value;
    const auto slot_b = SlottedPage::insert(page, 4, b).value;
    const auto slot_c = SlottedPage::insert(page, 4, c).value;
    bool passed = expect(slot_a && slot_b && slot_c, "delete setup inserts must succeed");
    passed = expect(!SlottedPage::erase(page, 4, *slot_b).has_value(),
                    "deleting middle record must succeed") && passed;
    passed = expect(read_u16(page, kLiveCountOffset) == 2 &&
                        read_u16(page, kSlotCountOffset) == 3 &&
                        read_u16(page, kFreeUpperOffset) == kPageSize - a.size() - c.size(),
                    "delete must compact payload while preserving slot_count") && passed;
    passed = expect(SlottedPage::get(page, 4, *slot_a).value == std::optional{a} &&
                        SlottedPage::get(page, 4, *slot_c).value == std::optional{c} &&
                        has_error(SlottedPage::get(page, 4, *slot_b),
                                  SlottedPageErrorKind::kInvalidArgument),
                    "compaction must preserve live SlotIds and invalidate deleted slot") && passed;

    passed = expect(!SlottedPage::erase(page, 4, *slot_a).has_value() &&
                        !SlottedPage::erase(page, 4, *slot_c).has_value(),
                    "remaining records must delete") && passed;
    passed = expect(read_u16(page, kLiveCountOffset) == 0 &&
                        read_u16(page, kSlotCountOffset) == 3 &&
                        read_u16(page, kFreeLowerOffset) == 56 &&
                        read_u16(page, kFreeUpperOffset) == 4096,
                    "empty churned page must retain slots and reset free_upper") && passed;
    passed = expect(!SlottedPage::validate(page, 4).has_value(),
                    "empty churned page must validate") && passed;
    return passed;
}

bool test_slot_reuse_generation_and_stale_handle() {
    RawPage page = initialized(5);
    const auto first = SlottedPage::insert(page, 5, payload(3, 1));
    bool passed = expect(first.value == std::optional<SlotHandle>{{0, 1}},
                         "first slot generation must be 1");
    passed = expect(!SlottedPage::erase(page, 5, *first.value).has_value(),
                    "slot must delete") && passed;
    const auto second_payload = payload(4, 9);
    const auto second = SlottedPage::insert(page, 5, second_payload);
    passed = expect(second.value == std::optional<SlotHandle>{{0, 2}},
                    "reused slot generation must increment") && passed;
    passed = expect(has_error(SlottedPage::get(page, 5, *first.value),
                              SlottedPageErrorKind::kInvalidArgument) &&
                        SlottedPage::get(page, 5, *second.value).value ==
                            std::optional{second_payload},
                    "stale generation must fail without affecting current record") && passed;
    return passed;
}

bool test_retired_slot_and_space_accounting() {
    RawPage page = initialized(6);
    const auto first = SlottedPage::insert(page, 6, payload(1, 1));
    if (!expect(first.value.has_value(), "retire setup insert must succeed")) {
        return false;
    }
    write_u16(page, slot_offset(0) + 4, std::numeric_limits<std::uint16_t>::max());
    bool passed = expect(!SlottedPage::validate(page, 6).has_value(),
                         "live generation 65535 must be valid");
    passed = expect(!SlottedPage::erase(page, 6, SlotHandle{0, 65535}).has_value(),
                    "deleting generation 65535 must succeed") && passed;
    passed = expect(read_u16(page, slot_offset(0) + 6) == kSlotRetired &&
                        read_u16(page, slot_offset(0)) == 0 &&
                        read_u16(page, slot_offset(0) + 2) == 0,
                    "generation 65535 deletion must retire and clear location") && passed;
    const auto next = SlottedPage::insert(page, 6, payload(1, 2));
    passed = expect(next.value == std::optional<SlotHandle>{{1, 1}},
                    "retired slot must never be reused") && passed;

    RawPage reusable = initialized(7);
    const auto exact = payload(kMaxRecordPayloadBytes, 3);
    const auto exact_slot = SlottedPage::insert(reusable, 7, exact);
    passed = expect(exact_slot.value.has_value(), "exact-fit first insert must succeed") && passed;
    passed = expect(!SlottedPage::erase(reusable, 7, *exact_slot.value).has_value(),
                    "exact-fit record must delete") && passed;
    passed = expect(SlottedPage::insert(reusable, 7, exact).value ==
                        std::optional<SlotHandle>{{0, 2}},
                    "reusable slot needs payload bytes but no new Slot Entry") && passed;

    RawPage retired = initialized(8);
    const auto retired_first = SlottedPage::insert(retired, 8, payload(1, 1));
    write_u16(retired, slot_offset(0) + 4, 65535);
    SlottedPage::erase(retired, 8, SlotHandle{0, 65535});
    passed = expect(has_error(SlottedPage::insert(retired, 8, exact),
                              SlottedPageErrorKind::kNoSpace),
                    "new slot must include 8-byte Slot Entry in space requirement") && passed;
    return passed;
}

bool test_exact_fit_and_full_page() {
    RawPage page = initialized(9);
    const auto exact = payload(kMaxRecordPayloadBytes, 11);
    const auto inserted = SlottedPage::insert(page, 9, exact);
    bool passed = expect(inserted.value.has_value() &&
                             read_u16(page, kFreeLowerOffset) ==
                                 read_u16(page, kFreeUpperOffset),
                         "4056-byte payload must exactly fill an empty page");
    passed = expect(has_error(SlottedPage::insert(page, 9, payload(1, 1)),
                              SlottedPageErrorKind::kNoSpace),
                    "full page must return no-space") && passed;
    passed = expect(has_error(SlottedPage::insert(
                                  page, 9, payload(kMaxRecordPayloadBytes + 1, 1)),
                              SlottedPageErrorKind::kInvalidArgument),
                    "payload one byte beyond maximum must be invalid") && passed;
    passed = expect(has_error(SlottedPage::insert(page, 9, {}),
                              SlottedPageErrorKind::kInvalidArgument),
                    "empty payload must be invalid") && passed;
    return passed;
}

template <typename Mutator>
bool corruption_is_rejected(Mutator mutate) {
    RawPage page = initialized(20);
    const auto first = SlottedPage::insert(page, 20, payload(4, 1));
    const auto second = SlottedPage::insert(page, 20, payload(5, 10));
    if (!first.value || !second.value) {
        return false;
    }
    mutate(page);
    return has_error(SlottedPage::validate(page, 20), SlottedPageErrorKind::kCorrupt);
}

bool test_header_corruption() {
    bool passed = expect(corruption_is_rejected([](RawPage& page) {
                             page.bytes[0] = std::byte{'X'};
                         }), "corrupt magic must fail");
    passed = expect(corruption_is_rejected([](RawPage& page) { write_u16(page, 4, 2); }),
                    "corrupt version must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) { write_u16(page, 6, 31); }),
                    "corrupt header size must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) { write_u32(page, 8, 21); }),
                    "self PageId mismatch must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, kFreeLowerOffset, 32);
                     }), "incorrect free_lower must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, kFreeUpperOffset, 31);
                     }), "invalid free_upper must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, kLiveCountOffset, 3);
                     }), "live_count beyond slots must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         page.bytes[20] = std::byte{1};
                     }), "nonzero reserved byte must fail") && passed;
    return passed;
}

bool test_slot_corruption() {
    bool passed = expect(corruption_is_rejected([](RawPage& page) {
                             write_u16(page, slot_offset(0) + 6, 0x8000);
                         }), "unknown flags must fail");
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, slot_offset(0) + 6,
                                   kSlotOccupied | kSlotRetired);
                     }), "occupied and retired flags must conflict") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, slot_offset(0) + 4, 0);
                     }), "live generation zero must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, slot_offset(1), read_u16(page, slot_offset(0)));
                     }), "overlapping payloads must fail") && passed;
    passed = expect(corruption_is_rejected([](RawPage& page) {
                         write_u16(page, slot_offset(0), 31);
                     }), "payload outside valid range must fail") && passed;

    RawPage deleted = initialized(21);
    const auto handle = SlottedPage::insert(deleted, 21, payload(2, 1));
    if (handle.value) {
        SlottedPage::erase(deleted, 21, *handle.value);
        write_u16(deleted, slot_offset(0), 100);
    }
    passed = expect(has_error(SlottedPage::validate(deleted, 21),
                              SlottedPageErrorKind::kCorrupt),
                    "deleted slot with nonzero offset must fail") && passed;
    return passed;
}

}  // namespace

int main() {
    const bool passed = test_initialize_layout() && test_insert_get_and_multiple_records() &&
                        test_delete_compact_and_delete_all() &&
                        test_slot_reuse_generation_and_stale_handle() &&
                        test_retired_slot_and_space_accounting() &&
                        test_exact_fit_and_full_page() && test_header_corruption() &&
                        test_slot_corruption();
    return passed ? 0 : 1;
}
