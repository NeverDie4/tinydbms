#ifndef TINYDBMS_TEST_COMPILER_FAKE_HPP
#define TINYDBMS_TEST_COMPILER_FAKE_HPP

#include <cstddef>
#include <deque>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace tinydbms::testing::fake_compiler {

struct State {
    std::size_t split_calls = 0;
    std::size_t compile_calls = 0;
    std::vector<std::size_t> catalog_sizes;
    std::deque<compiler::CompileResult> compile_results;
};

void reset();
State& state();
void set_compile_results(std::deque<compiler::CompileResult> results);

}  // namespace tinydbms::testing::fake_compiler

#endif  // TINYDBMS_TEST_COMPILER_FAKE_HPP
