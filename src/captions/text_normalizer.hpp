#pragma once
// Text clean-up for captions: strips recogniser annotations, fixes capitalisation/punctuation,
// masks profanity (optional) and splits into lines/pages for on-screen display. UTF-8 aware for
// the Latin-1 range (enough for pt-BR/es/fr/it/de); other scripts pass through untouched.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hb::captions {

struct NormalizeOptions {
    bool capitalize = true;
    bool finalPunctuation = true;   // add "." to finals that end without . ! ? …
    bool profanityFilter = false;
    std::size_t maxCharsPerLine = 42;
    std::size_t maxLines = 2;
};

// Removes [..] (..) annotations such as [BLANK_AUDIO] or (música), music notes, control characters;
// collapses whitespace; trims. Never throws.
std::string cleanText(std::string_view text);

// cleanText + profanity mask + capitalisation + (final only) terminal punctuation. No line breaks.
std::string normalizeCaption(std::string_view text, const NormalizeOptions& opt, bool isFinal);

// Greedy word wrap by Unicode code points; words longer than maxChars are hard-split.
std::vector<std::string> wrapLines(std::string_view text, std::size_t maxChars);

// wrapLines grouped into pages of at most maxLines lines (joined with '\n').
std::vector<std::string> paginate(std::string_view text, std::size_t maxChars, std::size_t maxLines);

std::size_t utf8Length(std::string_view s);   // code points (invalid bytes count as one each)
std::vector<std::string> splitWords(std::string_view s);   // whitespace separated

}  // namespace hb::captions
