#ifndef TINYDBMS_APP_OUTPUT_HPP
#define TINYDBMS_APP_OUTPUT_HPP

#include <iosfwd>
#include <string>
#include <string_view>

#include "tinydbms/core.hpp"

namespace tinydbms::app {

struct RenderResult {
    bool output_ok = true;
    bool had_error = false;
};

std::string escape_text(std::string_view text);

bool write_error(const tinydbms::core::Error& error, std::ostream& output);

RenderResult render_execute_result(
    const tinydbms::core::ExecuteResult& result,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_OUTPUT_HPP
