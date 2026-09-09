#include "input.hpp"

#include <exception>
#include <istream>
#include <string>
#include <utility>

namespace tinydbms::app {
namespace {

std::string exception_message(const std::exception& exception) {
    return exception.what() == nullptr ? "unknown input exception" : exception.what();
}

}  // namespace

bool read_batch(std::istream& input, std::string& text, std::string& error_message) {
    try {
        std::string buffer;
        char character = '\0';
        while (input.get(character)) {
            buffer.push_back(character);
        }
        if (input.bad() || (input.fail() && !input.eof())) {
            error_message = "failed to read stdin";
            return false;
        }
        text = std::move(buffer);
        return true;
    } catch (const std::exception& exception) {
        error_message = exception_message(exception);
        return false;
    } catch (...) {
        error_message = "unknown input exception";
        return false;
    }
}

bool read_line(
    std::istream& input,
    std::string& line,
    bool& reached_eof,
    std::string& error_message) {
    reached_eof = false;
    try {
        if (std::getline(input, line)) {
            return true;
        }
        if (input.bad()) {
            error_message = "failed to read stdin";
            return false;
        }
        if (input.eof()) {
            reached_eof = true;
            return true;
        }
        error_message = "failed to read stdin";
        return false;
    } catch (const std::exception& exception) {
        error_message = exception_message(exception);
        return false;
    } catch (...) {
        error_message = "unknown input exception";
        return false;
    }
}

}  // namespace tinydbms::app
