#pragma once

#include "page_types.h"
#include "storage_constants.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tinydbms::storage::internal {

using SlotId = std::uint16_t;

inline constexpr std::size_t kSlottedPageHeaderSize = 32;
inline constexpr std::size_t kSlotEntrySize = 8;
inline constexpr std::size_t kMaxSlotsPerPage =
    (kPageSize - kSlottedPageHeaderSize) / kSlotEntrySize;
inline constexpr std::uint16_t kSlotOccupied = 0x0001;
inline constexpr std::uint16_t kSlotRetired = 0x0002;
inline constexpr std::uint16_t kKnownSlotFlags = kSlotOccupied | kSlotRetired;

struct SlotHandle {
    SlotId slot_id;
    std::uint16_t generation;

    bool operator==(const SlotHandle&) const = default;
};

enum class SlottedPageErrorKind {
    kInvalidArgument,
    kNoSpace,
    kCorrupt,
};

struct SlottedPageError {
    SlottedPageErrorKind kind;
    std::string message;
};

template <typename T>
struct SlottedPageResult {
    std::optional<T> value;
    std::optional<SlottedPageError> error;
};

struct LiveSlotResult {
    std::optional<SlotHandle> slot;
    std::size_t next_position;
    std::optional<SlottedPageError> error;
};

class SlottedPage {
public:
    static LiveSlotResult next_live_slot(const RawPage&, PageId, std::size_t start_position);
    static std::optional<SlottedPageError> initialize(RawPage& page, PageId page_id);
    static std::optional<SlottedPageError> validate(const RawPage& page, PageId page_id);
    static SlottedPageResult<SlotHandle> insert(
        RawPage& page, PageId page_id, std::span<const std::byte> payload);
    static SlottedPageResult<std::vector<std::byte>> get(
        const RawPage& page, PageId page_id, SlotHandle handle);
    static std::optional<SlottedPageError> erase(
        RawPage& page, PageId page_id, SlotHandle handle);
};

}  // namespace tinydbms::storage::internal
