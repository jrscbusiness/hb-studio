#include "remote/commands.hpp"

#include <cmath>
#include <cstdlib>

namespace hb::remote {

namespace {
using core::json::Value;

Translation bad(const char* why) {
    Translation t;
    t.error = why;
    return t;
}
Translation good(std::string c) {
    Translation t;
    t.ok = true;
    t.hostCommand = std::move(c);
    return t;
}

bool getInt(const Value& o, const char* key, long long lo, long long hi, long long& out) {
    const Value* v = o.get(key);
    if (!v || !v->isNum() || !std::isfinite(v->n)) return false;
    if (v->n < static_cast<double>(lo) || v->n > static_cast<double>(hi)) return false;
    if (std::floor(v->n) != v->n) return false;  // integers only
    out = static_cast<long long>(v->n);
    return true;
}
bool getNum(const Value& o, const char* key, double lo, double hi, double& out) {
    const Value* v = o.get(key);
    if (!v || !v->isNum() || !std::isfinite(v->n) || v->n < lo || v->n > hi) return false;
    out = v->n;
    return true;
}
bool getBool(const Value& o, const char* key, bool& out) {
    const Value* v = o.get(key);
    if (!v || !v->isBool()) return false;
    out = v->b;
    return true;
}
// Locale independent fixed-point formatting of round(x * 10^decimals) / 10^decimals.
std::string fixed(double x, int decimals) {
    long long scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    long long q = std::llround(x * static_cast<double>(scale));
    const bool neg = q < 0;
    if (neg) q = -q;
    std::string frac = std::to_string(q % scale);
    while (static_cast<int>(frac.size()) < decimals) frac.insert(frac.begin(), '0');
    std::string s = std::string(neg ? "-" : "") + std::to_string(q / scale);
    if (decimals > 0) s += "." + frac;
    return s;
}
bool validDestId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (unsigned char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    return true;
}
}  // namespace

Translation translate(const Value& m, const CommandLimits& lim) {
    if (!m.isObj()) return bad("not_object");
    const Value* cv = m.get("c");
    if (!cv || !cv->isStr()) return bad("no_command");
    const std::string& c = cv->s;
    long long i = 0;
    bool on = false;
    if (c == "cut") return good("cut");
    if (c == "auto") {
        long long ms = lim.defaultAutoMs;
        if (m.get("ms") && !getInt(m, "ms", lim.minAutoMs, lim.maxAutoMs, ms)) return bad("bad_ms");
        return good("dissolve|" + std::to_string(ms));
    }
    if (c == "pvw" || c == "pgm") {
        if (!getInt(m, "i", 0, lim.maxSources - 1, i)) return bad("bad_source");
        return good((c == "pvw" ? "preview|" : "program|") + std::to_string(i));
    }
    if (c == "tbar") {
        double v;
        if (!getNum(m, "v", 0.0, 1.0, v)) return bad("bad_tbar");
        return good("tbar|" + fixed(v, 3));
    }
    if (c == "mute" || c == "solo") {
        if (!getInt(m, "ch", 0, lim.maxChannels - 1, i) || !getBool(m, "on", on)) return bad("bad_args");
        return good("aud|strip|" + std::to_string(i) + "|" + c + "|" + (on ? "1" : "0"));
    }
    if (c == "fader") {
        double db;
        if (!getInt(m, "ch", 0, lim.maxChannels - 1, i) || !getNum(m, "db", -200.0, 10.0, db)) return bad("bad_args");
        if (db > 6.0) db = 6.0;       // a phone never pushes the fader into the hot zone
        if (db <= -60.0) db = -120.0; // bottom of the travel = -inf (same convention as the desktop UI)
        return good("aud|strip|" + std::to_string(i) + "|faderDb|" + fixed(db, 1));
    }
    if (c == "rec") {
        if (!getBool(m, "on", on)) return bad("bad_args");
        return good(on ? "rec|start" : "rec|stop");
    }
    if (c == "xmit") {
        const Value* id = m.get("id");
        if (!id || !id->isStr() || !validDestId(id->s) || !getBool(m, "on", on)) return bad("bad_args");
        return good(std::string(on ? "xmitstart|" : "xmitstop|") + id->s);
    }
    if (c == "tp") {
        const Value* a = m.get("a");
        if (!a || !a->isStr()) return bad("bad_args");
        if (a->s == "play") return good("tp|play");
        if (a->s == "pause") return good("tp|pause");
        if (a->s == "speed" || a->s == "seek") {
            double v;
            if (!getNum(m, "v", -1.0e6, 1.0e6, v)) return bad("bad_args");
            return good("tp|" + a->s + "|" + fixed(v, 3));
        }
        return bad("bad_args");
    }
    return bad("not_allowed");
}

}  // namespace hb::remote
