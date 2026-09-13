#ifndef TINYDBMS_CORE_DIAGNOSTICS_HPP
#define TINYDBMS_CORE_DIAGNOSTICS_HPP

#include "tinydbms/compiler.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace tinydbms::core::internal {

// 空插入点 {1,1,0}-{1,1,0}：用于没有可信局部位置的脚本级错误。
SourceRange empty_source_range() noexcept;

// 校验单个范围：行列与偏移一致、偏移不越界、端点不落在 \r\n 之间。
bool is_valid_source_range(const SourceRange& range, std::string_view text) noexcept;

// 把 compile() 返回的语句相对范围换算为脚本绝对范围。
// statement.source 与 statement.sql 必须一一对应，relative 必须落在 statement.sql 内；
// 任何契约违反都返回 nullopt，由调用方转换为 kInternal。
std::optional<SourceRange> absolutize_range(
    const compiler::SplitStatement& statement,
    const SourceRange& relative,
    std::string_view script_text) noexcept;

}  // namespace tinydbms::core::internal

#endif  // TINYDBMS_CORE_DIAGNOSTICS_HPP
