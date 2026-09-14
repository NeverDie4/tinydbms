#ifndef TINYDBMS_TEST_COMPILER_FAKE_HPP
#define TINYDBMS_TEST_COMPILER_FAKE_HPP

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace tinydbms::testing::fake_compiler {

struct State {
    std::size_t split_calls = 0;
    std::size_t compile_calls = 0;
    std::vector<std::size_t> catalog_sizes;
    // 每次 compile() 看到的 Catalog TableId 快照，用于断言影子分配的候选 ID。
    std::vector<std::vector<TableId>> catalog_table_ids;
    std::deque<compiler::CompileResult> compile_results;
    // 非 0 时，第 N 次 compile() 调用抛出异常，用于测试致命中止路径。
    std::size_t throw_on_compile_call = 0;
    // 为 true 时 split_statements() 抛出异常。
    bool throw_on_split = false;
    // 非空时 split_statements() 直接返回该结果，用于构造契约违反场景。
    std::optional<std::vector<compiler::SplitStatement>> split_override;
};

void reset();
State& state();
void set_compile_results(std::deque<compiler::CompileResult> results);

// 以文本内字节偏移构造行列一致的半开范围，避免测试手算行列。
SourceRange make_range(
    std::string_view text,
    std::size_t begin_offset,
    std::size_t end_offset);

// 用语句内子串首次出现的位置构造范围；找不到时返回整个文本范围。
SourceRange make_range_of(std::string_view text, std::string_view needle);

// 返回整段脚本里第 index 条语句的分段原文（含前导空白/注释），语义与
// split_statements 一致且不影响调用计数；越界时返回空串。
// 构造 compile() 诊断时必须以它为基准计算相对偏移。
std::string statement_segment(std::string_view text, std::size_t index);

// 构造一个相对单条语句的编译错误。
compiler::CompileError make_compile_error(
    CompileStage stage,
    std::string_view statement_sql,
    std::size_t begin_offset,
    std::size_t end_offset,
    std::string message,
    std::optional<std::string> suggestion = std::nullopt,
    std::optional<FixIt> fix_it = std::nullopt);

// 注入分句结果；传 nullopt 恢复默认的按分号切分。
void set_split_override(std::optional<std::vector<compiler::SplitStatement>> statements);

}  // namespace tinydbms::testing::fake_compiler

#endif  // TINYDBMS_TEST_COMPILER_FAKE_HPP
