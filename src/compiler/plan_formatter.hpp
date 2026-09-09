#ifndef TINYDBMS_COMPILER_PLAN_FORMATTER_HPP
#define TINYDBMS_COMPILER_PLAN_FORMATTER_HPP

#include <string>

#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

[[nodiscard]] std::string format_plan(const Plan& plan);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_PLAN_FORMATTER_HPP
