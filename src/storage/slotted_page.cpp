#include "slotted_page.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace tinydbms::storage::internal {
namespace {

constexpr std::array<char, 4> kMagic{'T', 'S', 'P', '1'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kHeaderSizeOffset = 6;
constexpr std::size_t kPageIdOffset = 8;
constexpr std::size_t kSlotCountOffset = 12;
constexpr std::size_t kLiveCountOffset = 14;
constexpr std::size_t kFreeLowerOffset = 16;
constexpr std::size_t kFreeUpperOffset = 18;
constexpr std::size_t kReservedOffset = 20;

struct SlotEntry {
    std::uint16_t record_offset = 0;
    std::uint16_t record_length = 0;
    std::uint16_t generation = 0;
    std::uint16_t flags = 0;
};

SlottedPageError make_error(SlottedPageErrorKind kind, std::string message) {
    return SlottedPageError{kind, std::move(message)};
}

template <typename T>
SlottedPageResult<T> failure(SlottedPageErrorKind kind, std::string message) {
    return SlottedPageResult<T>{std::nullopt, make_error(kind, std::move(message))};
}

template <typename T>
SlottedPageResult<T> success(T value) {
    return SlottedPageResult<T>{std::move(value), std::nullopt};
}

std::uint16_t get_u16(const RawPage& page, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(page.bytes[offset])) |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(page.bytes[offset + 1]) << 8U);
}

std::uint32_t get_u32(const RawPage& page, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(page.bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

void put_u16(RawPage& page, std::size_t offset, std::uint16_t value) {
    page.bytes[offset] = std::byte{static_cast<std::uint8_t>(value & 0xffU)};
    page.bytes[offset + 1] =
        std::byte{static_cast<std::uint8_t>((value >> 8U) & 0xffU)};
}

void put_u32(RawPage& page, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page.bytes[offset + index] =
            std::byte{static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU)};
    }
}

std::size_t slot_byte_offset(SlotId slot_id) {
    return kSlottedPageHeaderSize + static_cast<std::size_t>(slot_id) * kSlotEntrySize;
}

SlotEntry read_slot(const RawPage& page, SlotId slot_id) {
    const std::size_t offset = slot_byte_offset(slot_id);
    return SlotEntry{get_u16(page, offset), get_u16(page, offset + 2),
                     get_u16(page, offset + 4), get_u16(page, offset + 6)};
}

void write_slot(RawPage& page, SlotId slot_id, const SlotEntry& slot) {
    const std::size_t offset = slot_byte_offset(slot_id);
    put_u16(page, offset, slot.record_offset);
    put_u16(page, offset + 2, slot.record_length);
    put_u16(page, offset + 4, slot.generation);
    put_u16(page, offset + 6, slot.flags);
}

bool has_magic(const RawPage& page) {
    return std::equal(kMagic.begin(), kMagic.end(), page.bytes.begin(),
                      [](char expected, std::byte actual) {
                          return static_cast<unsigned char>(expected) ==
                                 std::to_integer<unsigned char>(actual);
                      });
}

std::optional<SlottedPageError> invalid_handle(const SlotEntry& slot, SlotHandle handle) {
    if (handle.generation == 0 || slot.flags != kSlotOccupied ||
        slot.generation != handle.generation) {
        return make_error(SlottedPageErrorKind::kInvalidArgument,
                          "slot handle is stale or does not identify a live record");
    }
    return std::nullopt;
}

}  // namespace

LiveSlotResult SlottedPage::next_live_slot(const RawPage& page, PageId id, std::size_t start) {
    if (auto error = validate(page,id)) return {std::nullopt,start,std::move(error)};
    const auto count = get_u16(page,kSlotCountOffset);
    if (start > count) return {std::nullopt,start,
        make_error(SlottedPageErrorKind::kInvalidArgument,"scan position exceeds slot directory")};
    for (std::size_t i=start; i<count; ++i) {
        auto slot=read_slot(page,static_cast<SlotId>(i));
        if (slot.flags==kSlotOccupied)
            return {SlotHandle{static_cast<SlotId>(i),slot.generation},i+1,std::nullopt};
    }
    return {std::nullopt,count,std::nullopt};
}

std::optional<SlottedPageError> SlottedPage::initialize(RawPage& page, PageId page_id) {
    if (page_id == 0) {
        return make_error(SlottedPageErrorKind::kInvalidArgument,
                          "PageId 0 cannot be a SlottedPage");
    }
    page = RawPage{};
    std::transform(kMagic.begin(), kMagic.end(), page.bytes.begin(),
                   [](char value) { return std::byte{static_cast<unsigned char>(value)}; });
    put_u16(page, kVersionOffset, kFormatVersion);
    put_u16(page, kHeaderSizeOffset, static_cast<std::uint16_t>(kSlottedPageHeaderSize));
    put_u32(page, kPageIdOffset, page_id);
    put_u16(page, kFreeLowerOffset, static_cast<std::uint16_t>(kSlottedPageHeaderSize));
    put_u16(page, kFreeUpperOffset, static_cast<std::uint16_t>(kPageSize));
    return std::nullopt;
}

std::optional<SlottedPageError> SlottedPage::validate(const RawPage& page, PageId page_id) {
    if (page_id == 0) {
        return make_error(SlottedPageErrorKind::kInvalidArgument,
                          "PageId 0 cannot identify a SlottedPage");
    }
    if (!has_magic(page) || get_u16(page, kVersionOffset) != kFormatVersion ||
        get_u16(page, kHeaderSizeOffset) != kSlottedPageHeaderSize ||
        get_u32(page, kPageIdOffset) != page_id) {
        return make_error(SlottedPageErrorKind::kCorrupt,
                          "SlottedPage header identity is invalid");
    }
    if (!std::all_of(page.bytes.begin() + kReservedOffset,
                     page.bytes.begin() + kSlottedPageHeaderSize,
                     [](std::byte value) { return value == std::byte{0}; })) {
        return make_error(SlottedPageErrorKind::kCorrupt,
                          "SlottedPage reserved header bytes are not zero");
    }

    const std::uint16_t slot_count = get_u16(page, kSlotCountOffset);
    const std::uint16_t live_count = get_u16(page, kLiveCountOffset);
    const std::uint16_t free_lower = get_u16(page, kFreeLowerOffset);
    const std::uint16_t free_upper = get_u16(page, kFreeUpperOffset);
    const std::size_t expected_free_lower =
        kSlottedPageHeaderSize + static_cast<std::size_t>(slot_count) * kSlotEntrySize;
    if (slot_count > kMaxSlotsPerPage || live_count > slot_count ||
        free_lower != expected_free_lower || free_lower < kSlottedPageHeaderSize ||
        free_lower > free_upper || free_upper > kPageSize ||
        (live_count == 0 && free_upper != kPageSize)) {
        return make_error(SlottedPageErrorKind::kCorrupt,
                          "SlottedPage free-space counters are invalid");
    }

    struct Range {
        std::uint16_t begin;
        std::uint16_t end;
    };
    std::vector<Range> ranges;
    ranges.reserve(live_count);
    std::size_t counted_live = 0;
    for (std::size_t index = 0; index < slot_count; ++index) {
        const SlotEntry slot = read_slot(page, static_cast<SlotId>(index));
        if ((slot.flags & ~kKnownSlotFlags) != 0 ||
            slot.flags == (kSlotOccupied | kSlotRetired)) {
            return make_error(SlottedPageErrorKind::kCorrupt,
                              "Slot Entry contains invalid flags");
        }
        if (slot.flags == kSlotOccupied) {
            if (slot.generation == 0 || slot.record_offset == 0 || slot.record_length == 0 ||
                slot.record_offset < free_upper || slot.record_offset < free_lower ||
                static_cast<std::size_t>(slot.record_offset) + slot.record_length > kPageSize) {
                return make_error(SlottedPageErrorKind::kCorrupt,
                                  "live Slot Entry contains an invalid payload range");
            }
            ++counted_live;
            ranges.push_back(Range{slot.record_offset,
                                   static_cast<std::uint16_t>(slot.record_offset +
                                                              slot.record_length)});
        } else if (slot.flags == kSlotRetired) {
            if (slot.generation != std::numeric_limits<std::uint16_t>::max() ||
                slot.record_offset != 0 || slot.record_length != 0) {
                return make_error(SlottedPageErrorKind::kCorrupt,
                                  "retired Slot Entry is invalid");
            }
        } else if (slot.generation == 0 ||
                   slot.generation == std::numeric_limits<std::uint16_t>::max() ||
                   slot.record_offset != 0 || slot.record_length != 0) {
            return make_error(SlottedPageErrorKind::kCorrupt,
                              "deleted Slot Entry is invalid");
        }
    }
    if (counted_live != live_count) {
        return make_error(SlottedPageErrorKind::kCorrupt,
                          "live_count does not match occupied slots");
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const Range& left, const Range& right) { return left.begin < right.begin; });
    std::size_t next = free_upper;
    for (const Range& range : ranges) {
        if (range.begin != next || range.end <= range.begin) {
            return make_error(SlottedPageErrorKind::kCorrupt,
                              "record payload ranges overlap or contain gaps");
        }
        next = range.end;
    }
    if (next != kPageSize) {
        return make_error(SlottedPageErrorKind::kCorrupt,
                          "record payload area is not compact");
    }
    return std::nullopt;
}

SlottedPageResult<SlotHandle> SlottedPage::insert(
    RawPage& page, PageId page_id, std::span<const std::byte> payload) {
    if (payload.empty() || payload.size() > kMaxRecordPayloadBytes) {
        return failure<SlotHandle>(SlottedPageErrorKind::kInvalidArgument,
                                   "record payload size is invalid");
    }
    if (auto validation_error = validate(page, page_id); validation_error.has_value()) {
        return SlottedPageResult<SlotHandle>{std::nullopt, std::move(validation_error)};
    }

    const std::uint16_t slot_count = get_u16(page, kSlotCountOffset);
    SlotId target = 0;
    bool reuse = false;
    for (std::size_t index = 0; index < slot_count; ++index) {
        const SlotEntry slot = read_slot(page, static_cast<SlotId>(index));
        if (slot.flags == 0 && slot.generation < std::numeric_limits<std::uint16_t>::max()) {
            target = static_cast<SlotId>(index);
            reuse = true;
            break;
        }
    }
    if (!reuse) {
        if (slot_count >= kMaxSlotsPerPage) {
            return failure<SlotHandle>(SlottedPageErrorKind::kNoSpace,
                                       "SlottedPage has no room for another Slot Entry");
        }
        target = slot_count;
    }

    const std::uint16_t free_lower = get_u16(page, kFreeLowerOffset);
    const std::uint16_t free_upper = get_u16(page, kFreeUpperOffset);
    const std::size_t required = payload.size() + (reuse ? 0 : kSlotEntrySize);
    if (static_cast<std::size_t>(free_upper - free_lower) < required) {
        return failure<SlotHandle>(SlottedPageErrorKind::kNoSpace,
                                   "SlottedPage has insufficient contiguous free space");
    }

    RawPage updated = page;
    SlotEntry slot;
    if (reuse) {
        slot = read_slot(page, target);
        ++slot.generation;
    } else {
        slot.generation = 1;
        put_u16(updated, kSlotCountOffset, static_cast<std::uint16_t>(slot_count + 1));
        put_u16(updated, kFreeLowerOffset,
                static_cast<std::uint16_t>(free_lower + kSlotEntrySize));
    }
    slot.record_offset = static_cast<std::uint16_t>(free_upper - payload.size());
    slot.record_length = static_cast<std::uint16_t>(payload.size());
    slot.flags = kSlotOccupied;
    std::copy(payload.begin(), payload.end(), updated.bytes.begin() + slot.record_offset);
    write_slot(updated, target, slot);
    put_u16(updated, kLiveCountOffset,
            static_cast<std::uint16_t>(get_u16(page, kLiveCountOffset) + 1));
    put_u16(updated, kFreeUpperOffset, slot.record_offset);

    if (auto validation_error = validate(updated, page_id); validation_error.has_value()) {
        return SlottedPageResult<SlotHandle>{std::nullopt, std::move(validation_error)};
    }
    page = std::move(updated);
    return success(SlotHandle{target, slot.generation});
}

SlottedPageResult<std::vector<std::byte>> SlottedPage::get(
    const RawPage& page, PageId page_id, SlotHandle handle) {
    if (auto validation_error = validate(page, page_id); validation_error.has_value()) {
        return SlottedPageResult<std::vector<std::byte>>{std::nullopt,
                                                         std::move(validation_error)};
    }
    const std::uint16_t slot_count = get_u16(page, kSlotCountOffset);
    if (handle.slot_id >= slot_count) {
        return failure<std::vector<std::byte>>(SlottedPageErrorKind::kInvalidArgument,
                                               "SlotId is outside the Slot Directory");
    }
    const SlotEntry slot = read_slot(page, handle.slot_id);
    if (auto handle_error = invalid_handle(slot, handle); handle_error.has_value()) {
        return SlottedPageResult<std::vector<std::byte>>{std::nullopt,
                                                         std::move(handle_error)};
    }
    return success(std::vector<std::byte>(page.bytes.begin() + slot.record_offset,
                                          page.bytes.begin() + slot.record_offset +
                                              slot.record_length));
}

std::optional<SlottedPageError> SlottedPage::erase(
    RawPage& page, PageId page_id, SlotHandle handle) {
    if (auto validation_error = validate(page, page_id); validation_error.has_value()) {
        return validation_error;
    }
    const std::uint16_t slot_count = get_u16(page, kSlotCountOffset);
    if (handle.slot_id >= slot_count) {
        return make_error(SlottedPageErrorKind::kInvalidArgument,
                          "SlotId is outside the Slot Directory");
    }

    std::vector<SlotEntry> slots;
    slots.reserve(slot_count);
    for (std::size_t index = 0; index < slot_count; ++index) {
        slots.push_back(read_slot(page, static_cast<SlotId>(index)));
    }
    if (auto handle_error = invalid_handle(slots[handle.slot_id], handle);
        handle_error.has_value()) {
        return handle_error;
    }

    SlotEntry& deleted = slots[handle.slot_id];
    deleted.record_offset = 0;
    deleted.record_length = 0;
    deleted.flags = deleted.generation == std::numeric_limits<std::uint16_t>::max()
                        ? kSlotRetired
                        : 0;

    RawPage compacted;
    if (auto initialize_error = initialize(compacted, page_id); initialize_error.has_value()) {
        return initialize_error;
    }
    const std::uint16_t new_live_count =
        static_cast<std::uint16_t>(get_u16(page, kLiveCountOffset) - 1);
    put_u16(compacted, kSlotCountOffset, slot_count);
    put_u16(compacted, kLiveCountOffset, new_live_count);
    put_u16(compacted, kFreeLowerOffset,
            static_cast<std::uint16_t>(kSlottedPageHeaderSize +
                                       static_cast<std::size_t>(slot_count) * kSlotEntrySize));

    std::uint16_t cursor = static_cast<std::uint16_t>(kPageSize);
    for (std::size_t index = 0; index < slot_count; ++index) {
        SlotEntry& slot = slots[index];
        if (slot.flags == kSlotOccupied) {
            const std::uint16_t old_offset = slot.record_offset;
            cursor = static_cast<std::uint16_t>(cursor - slot.record_length);
            std::copy(page.bytes.begin() + old_offset,
                      page.bytes.begin() + old_offset + slot.record_length,
                      compacted.bytes.begin() + cursor);
            slot.record_offset = cursor;
        }
        write_slot(compacted, static_cast<SlotId>(index), slot);
    }
    put_u16(compacted, kFreeUpperOffset,
            new_live_count == 0 ? static_cast<std::uint16_t>(kPageSize) : cursor);

    if (auto validation_error = validate(compacted, page_id); validation_error.has_value()) {
        return validation_error;
    }
    page = std::move(compacted);
    return std::nullopt;
}

}  // namespace tinydbms::storage::internal
