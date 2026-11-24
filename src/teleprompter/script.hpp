#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Teleprompter script parsing. Pure text logic: no I/O, no platform code.
namespace hb::teleprompter {

// A script is plain UTF-8 text. Blank lines separate paragraphs; a line whose first non-blank
// character is '#' is a cue/comment for the operator: it is kept (shown to the operator) but is
// never part of what the presenter reads, and its words are not counted for timing.
struct Block {
    enum class Kind { Paragraph, Cue };
    Kind kind = Kind::Paragraph;
    std::string text;   // paragraph: trimmed lines joined with '\n'; cue: text after the '#' marks
    int words = 0;      // 0 for cues
    int line = 1;       // 1-based line of the block's first line in the source text
};

struct ParsedScript {
    std::vector<Block> blocks;
    int totalWords = 0;
    int paragraphs = 0;
};

// Maximum script size accepted anywhere (editor, import, host command).
constexpr std::size_t kMaxScriptBytes = 1024 * 1024;

// Whitespace-separated word count (ASCII whitespace; UTF-8 letters count as part of a word).
int countWords(std::string_view text);

ParsedScript parseScript(std::string_view text);

// For each Paragraph block (in order): the fraction 0..1 of the script's words that precede it.
// Used to place "jump to paragraph" targets proportionally to the reading time.
std::vector<double> paragraphStarts(const ParsedScript& script);

// Normalises text read from a file or the UI: strips a UTF-8 BOM and NUL bytes, converts CRLF/CR
// to LF, and if the bytes are not valid UTF-8 reinterprets them as Windows-1252 (the usual
// encoding of old Brazilian .txt files) and re-encodes as UTF-8. Truncates to kMaxScriptBytes on
// a character boundary.
std::string normalizeText(std::string_view raw);

bool isValidUtf8(std::string_view s);

}  // namespace hb::teleprompter
