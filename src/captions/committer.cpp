#include "captions/committer.hpp"

#include "captions/text_normalizer.hpp"

namespace hb::captions {
namespace {

std::string join(const std::vector<std::string>& w) {
    std::string s;
    for (const auto& x : w) {
        if (!s.empty()) s.push_back(' ');
        s += x;
    }
    return s;
}

std::vector<std::string> keys(const std::vector<std::string>& w) {
    std::vector<std::string> k;
    k.reserve(w.size());
    for (const auto& x : w) k.push_back(CaptionCommitter::wordKey(x));
    return k;
}

std::size_t commonPrefix(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
}

}  // namespace

std::string CaptionCommitter::wordKey(const std::string& word) {
    std::string k;
    for (std::size_t i = 0; i < word.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(word[i]);
        if (c >= 'A' && c <= 'Z') k.push_back(static_cast<char>(c + 32));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
            if (c == 0xC3 && i + 1 < word.size()) {   // Latin-1 capital letters -> lower case
                unsigned char d = static_cast<unsigned char>(word[i + 1]);
                if (d >= 0x80 && d <= 0x9E && d != 0x97) d = static_cast<unsigned char>(d + 0x20);
                k.push_back(static_cast<char>(c));
                k.push_back(static_cast<char>(d));
                ++i;
            } else {
                k.push_back(static_cast<char>(c));
            }
        }
        // ASCII punctuation is dropped
    }
    return k;
}

void CaptionCommitter::reset() {
    shownWords_.clear();
    shown_.clear();
    prevHyp_.clear();
    stable_ = 0;
}

CaptionCommitter::Update CaptionCommitter::onPartial(std::uint64_t utteranceId, const std::string& text) {
    Update up;
    if (utteranceId != id_) { reset(); id_ = utteranceId; }
    const auto cur = splitWords(text);
    if (cur.empty()) return up;
    const auto curKeys = keys(cur);
    const auto shownKeys = keys(shownWords_);

    stable_ = commonPrefix(keys(prevHyp_), curKeys);
    prevHyp_ = cur;

    // A hypothesis that is only a shorter prefix of what is already shown: do not retract.
    if (curKeys.size() <= shownKeys.size() && commonPrefix(curKeys, shownKeys) == curKeys.size()) return up;

    const std::size_t agreed = commonPrefix(shownKeys, curKeys);   // shown words still supported
    std::vector<std::string> next(shownWords_.begin(), shownWords_.begin() + static_cast<std::ptrdiff_t>(agreed));
    next.insert(next.end(), cur.begin() + static_cast<std::ptrdiff_t>(agreed), cur.end());
    const std::string joined = join(next);
    if (joined == shown_) return up;
    shownWords_ = std::move(next);
    shown_ = joined;
    up.emit = true;
    up.text = shown_;
    return up;
}

CaptionCommitter::Update CaptionCommitter::onFinal(std::uint64_t utteranceId, const std::string& text) {
    Update up;
    if (utteranceId != id_) { reset(); id_ = utteranceId; }
    const bool hadPartial = !shown_.empty();
    const auto words = splitWords(text);
    const std::string key = join(keys(words));
    reset();
    if (words.empty() || key.empty()) {
        if (hadPartial) up.emit = true;   // clear the stale partial
        return up;
    }
    // Only multi-word repeats are treated as recogniser loops: a speaker may legitimately say "Amém." twice.
    if (words.size() >= 3 && key == lastFinalKey_) {
        if (hadPartial) up.emit = true;
        return up;
    }
    lastFinalKey_ = key;
    up.emit = true;
    up.text = join(words);
    return up;
}

}  // namespace hb::captions
