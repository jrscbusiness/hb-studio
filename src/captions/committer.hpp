#pragma once
// Turns the stream of partial hypotheses of an utterance into stable on-screen text, then commits
// the utterance's final text. Word-level comparison ignores case and punctuation, so a recogniser
// flipping "olá," <-> "olá" or "Bom" <-> "bom" does not make the caption flicker:
//   * words already shown that the new hypothesis still agrees with keep the shown spelling,
//   * a hypothesis that is merely a shorter prefix of what is shown is ignored (no retraction),
//   * only a real change (new words, or a genuine rewrite of the tail) produces a new display text.
// Finals identical to the previous final (typical recogniser hallucination loops) are suppressed.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hb::captions {

class CaptionCommitter {
public:
    struct Update {
        bool emit = false;     // publish `text`?
        std::string text;      // may be empty with emit=true: clears a shown partial
    };

    // Text must already be cleaned (cleanText). Returns the text to display, if it changed.
    Update onPartial(std::uint64_t utteranceId, const std::string& text);
    // Commits the utterance. Empty/duplicate finals emit nothing, except that a shown partial is cleared.
    Update onFinal(std::uint64_t utteranceId, const std::string& text);

    std::size_t stableWords() const { return stable_; }   // words of the shown text agreed by 2 hypotheses
    const std::string& shown() const { return shown_; }
    void reset();

    static std::string wordKey(const std::string& word);   // lower-case, punctuation stripped

private:
    std::uint64_t id_ = 0;
    std::vector<std::string> shownWords_;
    std::string shown_;
    std::vector<std::string> prevHyp_;
    std::size_t stable_ = 0;
    std::string lastFinalKey_;
};

}  // namespace hb::captions
