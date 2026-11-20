#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Small, dependency-free JSON reader/writer helpers used for settings files and plugin manifests.
// The parser is strict about structure, bounded in depth/size (hostile files cannot overflow the
// stack or exhaust memory) and never throws.
namespace hb::core::json {

struct Value {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    bool isObj() const { return t == T::Obj; }
    bool isArr() const { return t == T::Arr; }
    bool isStr() const { return t == T::Str; }
    bool isNum() const { return t == T::Num; }
    bool isBool() const { return t == T::Bool; }
    const Value* get(const std::string& key) const {
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }
};

struct Limits {
    std::size_t maxBytes = 4 * 1024 * 1024;
    int maxDepth = 16;
    std::size_t maxArray = 4096;
};

// Returns false for any syntax error, trailing garbage, or limit violation.
bool parse(std::string_view text, Value& out, const Limits& limits = {});

// Serialises a value (object keys sorted). `pretty`: 2-space indentation.
std::string dump(const Value& v, bool pretty = false);

// JSON string escaping (no surrounding quotes). Control characters become \u00XX.
std::string escape(std::string_view s);

}  // namespace hb::core::json
