#ifndef TINYDBMS_DIAGNOSTIC_HPP
#define TINYDBMS_DIAGNOSTIC_HPP

#include <cstddef>
#include <string>

#include "tinydbms/common.hpp"

namespace tinydbms {

// 诊断范围统一使用半开区间 [begin, end)。
// 行列从 1 开始；offset 从 0 开始；列与偏移都按 UTF-8 字节计算。
struct SourceRange {
    SourceLocation begin;
    SourceLocation end;
    std::size_t begin_offset;
    std::size_t end_offset;
};

// 机器可用的单处替换建议：range 使用与主诊断相同的坐标系。
struct FixIt {
    SourceRange range;
    std::string replacement;
};

// 编译阶段同时用于 CompileError 与 core 的 Error，取代旧的 CompileErrorKind。
enum class CompileStage {
    kLex,
    kSyntax,
    kSemantic
};

}  // namespace tinydbms

#endif  // TINYDBMS_DIAGNOSTIC_HPP
