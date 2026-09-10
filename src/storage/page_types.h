#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tinydbms::storage::internal {

inline constexpr std::size_t kPageSize = 4096;
using PageId = std::uint32_t;

struct RawPage {
    std::array<std::byte, kPageSize> bytes{};
};

static_assert(sizeof(RawPage) == kPageSize);

}  // namespace tinydbms::storage::internal
