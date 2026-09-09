#ifndef TINYDBMS_APP_INPUT_HPP
#define TINYDBMS_APP_INPUT_HPP

#include <iosfwd>
#include <string>

namespace tinydbms::app {

bool read_batch(std::istream& input, std::string& text, std::string& error_message);

bool read_line(
    std::istream& input,
    std::string& line,
    bool& reached_eof,
    std::string& error_message);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_INPUT_HPP
