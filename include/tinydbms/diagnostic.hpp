#ifndef TINYDBMS_DIAGNOSTIC_HPP
#define TINYDBMS_DIAGNOSTIC_HPP

#include <string>

#include "tinydbms/common.hpp"

namespace tinydbms {

// Half-open [begin, end) source range. Line and UTF-8 byte column are 1-based;
// byte_offset is 0-based within the owning source text. CRLF is one newline,
// '\r' does not advance the column, tabs advance by one byte, and an endpoint
// must not fall between the two bytes of CRLF.
struct SourceRange {
    SourceLocation begin;
    SourceLocation end;
};

struct FixIt {
    SourceRange range;
    std::string replacement;
};

enum class CompileStage {
    kLex,
    kSyntax,
    kSemantic
};

}  // namespace tinydbms

#endif  // TINYDBMS_DIAGNOSTIC_HPP
