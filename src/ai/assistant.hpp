#pragma once
// Minimal optional AI tool: suggest a live title and description from a short topic.
#include <string>

namespace hb::ai {

struct Suggestion {
    bool ok = false;
    std::string title;
    std::string description;
};

std::string suggestSystemPrompt();
// The topic is untrusted user text: length-limited and wrapped, never executed as instructions by us.
std::string suggestUserPrompt(const std::string& topic);
// Tolerant parser: accepts raw JSON {"title","description"}, JSON inside ``` fences/prose, or
// "Título: ...\nDescrição: ..." / first-line-title fallback.
Suggestion parseSuggestion(const std::string& modelText);

}  // namespace hb::ai
