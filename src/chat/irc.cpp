#include "chat/irc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace hb::chat {

std::string unescapeIrcTag(std::string_view v) {
    std::string o;
    o.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\') { o += v[i]; continue; }
        if (i + 1 >= v.size()) break;  // dangling backslash is dropped (IRCv3 spec)
        switch (v[++i]) {
            case ':': o += ';'; break;
            case 's': o += ' '; break;
            case 'r': o += '\r'; break;
            case 'n': o += '\n'; break;
            case '\\': o += '\\'; break;
            default: o += v[i]; break;
        }
    }
    return o;
}

bool parseIrcLine(std::string_view line, IrcMessage& out) {
    out = IrcMessage{};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.empty() || line.size() > kMaxIrcLine) return false;
    std::size_t i = 0;
    auto skipSpaces = [&] { while (i < line.size() && line[i] == ' ') ++i; };

    if (line[i] == '@') {
        const std::size_t end = line.find(' ', i);
        if (end == std::string_view::npos) return false;
        std::string_view tags = line.substr(i + 1, end - i - 1);
        std::size_t p = 0;
        while (p <= tags.size()) {
            std::size_t q = tags.find(';', p);
            if (q == std::string_view::npos) q = tags.size();
            std::string_view kv = tags.substr(p, q - p);
            if (!kv.empty()) {
                const std::size_t eq = kv.find('=');
                std::string key(kv.substr(0, eq));
                if (!key.empty() && out.tags.size() < 128)
                    out.tags[key] = eq == std::string_view::npos ? "" : unescapeIrcTag(kv.substr(eq + 1));
            }
            p = q + 1;
        }
        i = end;
        skipSpaces();
    }
    if (i < line.size() && line[i] == ':') {
        const std::size_t end = line.find(' ', i);
        if (end == std::string_view::npos) return false;
        out.prefix.assign(line.substr(i + 1, end - i - 1));
        i = end;
        skipSpaces();
    }
    if (i >= line.size()) return false;
    {
        const std::size_t end = std::min(line.find(' ', i), line.size());
        out.command.assign(line.substr(i, end - i));
        for (auto& c : out.command) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        i = end;
    }
    if (out.command.empty()) return false;
    for (;;) {
        skipSpaces();
        if (i >= line.size()) break;
        if (line[i] == ':') {
            out.params.emplace_back(line.substr(i + 1));
            break;
        }
        const std::size_t end = std::min(line.find(' ', i), line.size());
        out.params.emplace_back(line.substr(i, end - i));
        i = end;
        if (out.params.size() >= 32) break;
    }
    return true;
}

std::vector<std::string> splitIrcLines(std::string_view chunk) {
    std::vector<std::string> lines;
    std::size_t p = 0;
    while (p < chunk.size()) {
        std::size_t q = chunk.find('\n', p);
        if (q == std::string_view::npos) q = chunk.size();
        std::string_view l = chunk.substr(p, q - p);
        while (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (!l.empty()) lines.emplace_back(l);
        p = q + 1;
    }
    return lines;
}

std::vector<EmoteRange> parseTwitchEmotes(std::string_view tag) {
    std::vector<EmoteRange> out;
    std::size_t p = 0;
    while (p < tag.size() && out.size() < 256) {
        std::size_t q = tag.find('/', p);
        if (q == std::string_view::npos) q = tag.size();
        std::string_view entry = tag.substr(p, q - p);
        p = q + 1;
        const std::size_t colon = entry.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        const std::string id(entry.substr(0, colon));
        std::string_view ranges = entry.substr(colon + 1);
        std::size_t r = 0;
        while (r < ranges.size() && out.size() < 256) {
            std::size_t e = ranges.find(',', r);
            if (e == std::string_view::npos) e = ranges.size();
            std::string_view rg = ranges.substr(r, e - r);
            r = e + 1;
            const std::size_t dash = rg.find('-');
            if (dash == std::string_view::npos) continue;
            const std::string a(rg.substr(0, dash)), b(rg.substr(dash + 1));
            if (a.empty() || b.empty() || a.size() > 6 || b.size() > 6) continue;
            if (!std::all_of(a.begin(), a.end(), ::isdigit) || !std::all_of(b.begin(), b.end(), ::isdigit)) continue;
            EmoteRange er;
            er.id = id;
            er.start = std::atoi(a.c_str());
            er.end = std::atoi(b.c_str());
            if (er.end < er.start) continue;
            out.push_back(std::move(er));
        }
    }
    return out;
}

unsigned parseTwitchBadges(std::string_view tag) {
    unsigned bits = 0;
    std::size_t p = 0;
    while (p < tag.size()) {
        std::size_t q = tag.find(',', p);
        if (q == std::string_view::npos) q = tag.size();
        std::string_view b = tag.substr(p, q - p);
        p = q + 1;
        b = b.substr(0, b.find('/'));
        if (b == "broadcaster") bits |= kBadgeOwner;
        else if (b == "moderator") bits |= kBadgeMod;
        else if (b == "subscriber" || b == "founder") bits |= kBadgeSub;
        else if (b == "verified") bits |= kBadgeVerified;
    }
    return bits;
}

std::optional<ChatMessage> twitchToChatMessage(const IrcMessage& m, std::int64_t receivedMs) {
    if (m.command != "PRIVMSG" || m.params.size() < 2) return std::nullopt;
    ChatMessage c;
    c.platform = Platform::Twitch;
    std::string text = m.params.back();
    // "/me text" arrives as CTCP ACTION: \x01ACTION text\x01
    if (text.size() >= 8 && text.front() == '\x01' && text.compare(1, 6, "ACTION") == 0) {
        text = text.substr(8);
        if (!text.empty() && text.back() == '\x01') text.pop_back();
    }
    c.text = sanitizeText(text, 500);
    if (c.text.empty()) return std::nullopt;
    auto tag = [&](const char* k) -> const std::string* {
        auto it = m.tags.find(k);
        return it == m.tags.end() ? nullptr : &it->second;
    };
    std::string nick;
    const std::size_t bang = m.prefix.find('!');
    if (bang != std::string::npos) nick = m.prefix.substr(0, bang);
    if (auto* dn = tag("display-name"); dn && !dn->empty()) c.author = sanitizeText(*dn, 64);
    else c.author = sanitizeText(nick, 64);
    if (c.author.empty()) return std::nullopt;
    if (auto* v = tag("id")) c.id = sanitizeText(*v, 64);
    if (auto* v = tag("user-id")) c.authorId = sanitizeText(*v, 32);
    if (c.authorId.empty()) c.authorId = sanitizeText(nick, 32);
    c.tsMs = receivedMs;
    if (auto* v = tag("tmi-sent-ts"); v && !v->empty() && v->size() <= 15 &&
                                      std::all_of(v->begin(), v->end(), ::isdigit))
        c.tsMs = std::atoll(v->c_str());
    if (auto* v = tag("badges")) c.badges = parseTwitchBadges(*v);
    if (auto* v = tag("mod"); v && *v == "1") c.badges |= kBadgeMod;
    if (auto* v = tag("subscriber"); v && *v == "1") c.badges |= kBadgeSub;
    if (auto* v = tag("emotes")) c.emotes = parseTwitchEmotes(*v);
    return c;
}

}  // namespace hb::chat
