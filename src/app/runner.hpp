#ifndef TINYDBMS_APP_RUNNER_HPP
#define TINYDBMS_APP_RUNNER_HPP

#include <iosfwd>
#include <string_view>

#include "session.hpp"

namespace tinydbms::app {

struct CliEnvironment {
    std::istream& input;
    std::ostream& output;
    std::ostream& error;
    bool interactive = false;
};

int run_cli(
    int argc,
    char* const argv[],
    std::string_view version,
    Session& session,
    CliEnvironment& environment);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_RUNNER_HPP
