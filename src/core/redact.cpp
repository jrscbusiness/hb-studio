#include "core/redact.hpp"

#include <regex>
#include <vector>

namespace hb::core {

namespace {
struct Rule {
    std::regex re;
    const char* replacement;
};

const std::vector<Rule>& rules() {
    using std::regex;
    constexpr auto icase = std::regex::ECMAScript | std::regex::icase;
    static const std::vector<Rule> r = {
        // Authorization header: mask the whole remainder of the line.
        {regex(R"(authorization\s*:[^\r\n]*)", icase), "Authorization: ***"},
        // scheme://user:pass@host  and  scheme://user@host
        {regex(R"(([a-z][a-z0-9+.\-]*://)[^/\s:@]*:[^/\s@]*@)", icase), "$1***:***@"},
        {regex(R"(([a-z][a-z0-9+.\-]*://)[^/\s:@]+@)", icase), "$1***@"},
        // key=value / key: value / "key":"value"
        {regex(R"(\b(password|passwd|pwd|passphrase|token|secret|api[_\-]?key|stream[_\-]?key|streamkey)\b(["']?\s*[:=]\s*["']?)[^\s"',;&]+)",
               icase),
         "$1$2***"},
        // <wsse:Password ...>value</wsse:Password>
        {regex(R"((<[^>]*password[^>]*>)[^<]*(</))", icase), "$1***$2"},
        // rtmp(s)://host/app/STREAMKEY
        {regex(R"((rtmps?://[^/\s]+/[^/\s]+/)[^\s?]+)", icase), "$1***"},
    };
    return r;
}
}  // namespace

std::string redactSecrets(std::string_view text) {
    std::string out(text);
    for (const auto& rule : rules())
        out = std::regex_replace(out, rule.re, rule.replacement);
    return out;
}

}  // namespace hb::core
