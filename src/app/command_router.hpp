#pragma once
// Text-command plumbing for the UI/remote bridge. Messages from the page look like "cmd|a|b|rest".
// Portable (no Windows headers) so it is unit-tested on Linux.
//
//   splitCommand(msg, maxParts)  - at most maxParts parts, the last keeps any further '|'
//   commandName(msg)             - the first token (before the first '|')
//   CommandRouter                - register(name -> handler); dispatch(msg) parses the first token once,
//                                  splits with the command's own part limit and calls the handler.
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hb::app {

// Splits "cmd|a|b|rest": at most `maxParts` parts, the last one keeps any further '|'.
// Always returns at least one element (an empty message yields {""}).
inline std::vector<std::string> splitCommand(const std::string& msg, std::size_t maxParts) {
    std::vector<std::string> a;
    std::size_t s0 = 0;
    for (std::size_t i = 0; i <= msg.size(); ++i)
        if (i == msg.size() || (msg[i] == '|' && a.size() + 1 < maxParts)) {
            a.push_back(msg.substr(s0, i - s0));
            s0 = i + 1;
        }
    return a;
}

inline std::string commandName(const std::string& msg) { return msg.substr(0, msg.find('|')); }

// A parsed command as seen by a handler. `a[0]` is the command name, `a[1..]` the arguments
// (same layout the hand-written `a = split(msg, n)` code used).
struct Command {
    const std::string& raw;
    std::vector<std::string> a;

    const std::string& name() const { return a[0]; }
    // Integer argument k (atoi semantics); -1 when the argument is missing.
    int num(std::size_t k) const { return k < a.size() ? std::atoi(a[k].c_str()) : -1; }
    // Argument k or "" when missing.
    std::string str(std::size_t k) const { return k < a.size() ? a[k] : std::string(); }
};

class CommandRouter {
public:
    // Handlers must be copy-constructible (they live in a std::function): capture by reference or
    // copy, never a move-only object.
    using Handler = std::function<void(const Command&)>;

    // Registers `name` (exact match of the first token). `minParts` counts the name itself: a message
    // with fewer parts is consumed (dispatch returns true) but the handler is NOT called, matching the
    // old `c == "x" && a.size() >= n` guards that silently ignored malformed messages.
    // `maxParts` is the split limit. A later registration of the same name replaces the earlier one.
    void add(const std::string& name, Handler handler, std::size_t minParts = 1, std::size_t maxParts = 8) {
        entries_[name] = Entry{std::move(handler), minParts, maxParts};
    }

    bool has(const std::string& name) const { return entries_.count(name) != 0; }

    // True if `msg`'s first token is a registered command (it was handled or deliberately ignored).
    bool dispatch(const std::string& msg) const {
        const auto it = entries_.find(commandName(msg));
        if (it == entries_.end()) return false;
        const Entry& e = it->second;
        Command cmd{msg, splitCommand(msg, e.maxParts)};
        if (cmd.a.size() >= e.minParts && e.handler) e.handler(cmd);
        return true;
    }

private:
    struct Entry {
        Handler handler;
        std::size_t minParts = 1;
        std::size_t maxParts = 8;
    };
    std::map<std::string, Entry> entries_;
};

}  // namespace hb::app
