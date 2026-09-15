#ifndef TINYDBMS_COMPILER_DIAGNOSTIC_SUGGESTION_HPP
#define TINYDBMS_COMPILER_DIAGNOSTIC_SUGGESTION_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinydbms::compiler::internal {

struct SuggestionCandidate {
    std::string_view spelling;
    std::string_view display;
};

[[nodiscard]] inline std::string normalize_suggestion_text(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char character : text) {
        normalized.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : character);
    }
    return normalized;
}

[[nodiscard]] inline std::size_t suggestion_distance_limit(std::size_t length) noexcept {
    if (length <= 4U) {
        return 1U;
    }
    if (length <= 8U) {
        return 2U;
    }
    return std::min<std::size_t>(3U, length / 3U);
}

[[nodiscard]] inline std::optional<std::size_t> bounded_damerau_levenshtein(
    std::string_view input,
    std::string_view candidate,
    std::size_t limit) {
    if (input.size() > candidate.size() + limit ||
        candidate.size() > input.size() + limit) {
        return std::nullopt;
    }

    std::vector<std::size_t> previous_previous(candidate.size() + 1U);
    std::vector<std::size_t> previous(candidate.size() + 1U);
    std::vector<std::size_t> current(candidate.size() + 1U);
    for (std::size_t column = 0; column <= candidate.size(); ++column) {
        previous[column] = column;
    }

    for (std::size_t row = 1; row <= input.size(); ++row) {
        current[0] = row;
        std::size_t row_minimum = current[0];
        for (std::size_t column = 1; column <= candidate.size(); ++column) {
            const std::size_t substitution_cost =
                input[row - 1U] == candidate[column - 1U] ? 0U : 1U;
            current[column] = std::min({
                previous[column] + 1U,
                current[column - 1U] + 1U,
                previous[column - 1U] + substitution_cost});
            if (row > 1U && column > 1U &&
                input[row - 1U] == candidate[column - 2U] &&
                input[row - 2U] == candidate[column - 1U]) {
                current[column] = std::min(
                    current[column], previous_previous[column - 2U] + 1U);
            }
            row_minimum = std::min(row_minimum, current[column]);
        }
        if (row_minimum > limit && row > candidate.size() + limit) {
            return std::nullopt;
        }
        previous_previous.swap(previous);
        previous.swap(current);
    }

    return previous[candidate.size()] <= limit
        ? std::optional<std::size_t>{previous[candidate.size()]}
        : std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> best_suggestion(
    std::string_view input,
    std::span<const SuggestionCandidate> candidates) {
    const std::string normalized_input = normalize_suggestion_text(input);
    const std::size_t limit = suggestion_distance_limit(normalized_input.size());
    std::size_t best_distance = std::numeric_limits<std::size_t>::max();
    std::optional<std::string> best;
    bool tied = false;

    for (const SuggestionCandidate& candidate : candidates) {
        const std::string normalized_candidate =
            normalize_suggestion_text(candidate.spelling);
        const std::optional<std::size_t> distance = bounded_damerau_levenshtein(
            normalized_input, normalized_candidate, limit);
        if (!distance.has_value()) {
            continue;
        }
        if (*distance < best_distance) {
            best_distance = *distance;
            best = std::string{candidate.display};
            tied = false;
        } else if (*distance == best_distance &&
                   best.has_value() && *best != candidate.display) {
            tied = true;
        }
    }
    return best.has_value() && !tied ? best : std::nullopt;
}

[[nodiscard]] inline std::string suggestion_message(std::string_view candidate) {
    return "did you mean '" + std::string{candidate} + "'?";
}

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_DIAGNOSTIC_SUGGESTION_HPP
